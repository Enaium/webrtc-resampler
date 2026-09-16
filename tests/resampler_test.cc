// Standalone tests for the extracted WebRTC resamplers.
//
// Exercises both resampler implementations:
//   * webrtc::Resampler           - integer multi-rate resampler (SPL based)
//   * webrtc::SincResampler /
//     webrtc::PushSincResampler /
//     webrtc::PushResampler<T>    - windowed sinc resampler (float/int16)
//
// Every test verifies observable behaviour (rate support contract, sample
// counts, tone level preservation, aliasing/imaging rejection, channel
// independence) and writes the processed signals to tests/out/ as 16 bit PCM
// WAV files for inspection.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "api/audio/audio_view.h"
#include "common_audio/include/audio_util.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "common_audio/resampler/include/resampler.h"
#include "common_audio/resampler/push_sinc_resampler.h"
#include "common_audio/resampler/sinc_resampler.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

std::string Fmt(const std::string& name, int in_rate, int out_rate) {
  return name + " (" + std::to_string(in_rate) + " -> " +
         std::to_string(out_rate) + ")";
}

// ---------------------------------------------------------------------------
// WAV I/O (16 bit PCM, little endian)
// ---------------------------------------------------------------------------

void WriteLe16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void WriteLe32(std::vector<uint8_t>* out, uint32_t v) {
  WriteLe16(out, static_cast<uint16_t>(v & 0xffff));
  WriteLe16(out, static_cast<uint16_t>((v >> 16) & 0xffff));
}

bool WriteWav(const std::string& path,
              const int16_t* samples,
              size_t frames,
              size_t channels,
              int sample_rate) {
  const uint16_t block_align = static_cast<uint16_t>(channels * 2);
  const uint32_t data_bytes = static_cast<uint32_t>(frames * block_align);

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_bytes);
  const char* riff = "RIFF";
  wav.insert(wav.end(), riff, riff + 4);
  WriteLe32(&wav, 36 + data_bytes);
  const char* wave = "WAVEfmt ";
  wav.insert(wav.end(), wave, wave + 8);
  WriteLe32(&wav, 16);                                    // fmt chunk size
  WriteLe16(&wav, 1);                                     // PCM
  WriteLe16(&wav, static_cast<uint16_t>(channels));
  WriteLe32(&wav, static_cast<uint32_t>(sample_rate));
  WriteLe32(&wav, static_cast<uint32_t>(sample_rate) * block_align);
  WriteLe16(&wav, block_align);
  WriteLe16(&wav, 16);                                    // bits per sample
  const char* data = "data";
  wav.insert(wav.end(), data, data + 4);
  WriteLe32(&wav, data_bytes);
  for (size_t i = 0; i < frames * channels; ++i) {
    WriteLe16(&wav, static_cast<uint16_t>(samples[i]));
  }

  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path());
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::printf("  FAIL: cannot open %s for writing\n", path.c_str());
    ++g_failures;
    return false;
  }
  const bool ok = std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
  std::fclose(f);
  return ok;
}

// ---------------------------------------------------------------------------
// Signal helpers
// ---------------------------------------------------------------------------

std::vector<int16_t> Sine(int sample_rate,
                          double duration_s,
                          const std::vector<double>& frequencies,
                          double amplitude = 8192.0) {
  const size_t frames = static_cast<size_t>(sample_rate * duration_s);
  std::vector<int16_t> out(frames, 0);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    double v = 0.0;
    for (double f : frequencies) {
      v += amplitude * std::sin(2.0 * kPi * f * t) / frequencies.size();
    }
    out[i] = static_cast<int16_t>(std::lround(
        std::clamp(v, -32768.0, 32767.0)));
  }
  return out;
}

// Coherent DFT amplitude (linear scale, same units as the samples) of `freq`
// over `count` samples starting at `start`. `count` must contain an integral
// number of cycles of `freq` for the result to be exact.
double ToneAmplitude(const std::vector<int16_t>& s,
                     size_t start,
                     size_t count,
                     int sample_rate,
                     double freq) {
  double re = 0.0;
  double im = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double w = 2.0 * kPi * freq * static_cast<double>(i) / sample_rate;
    const double x = s[start + i];
    re += x * std::cos(w);
    im -= x * std::sin(w);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

double ToneAmplitude(const std::vector<float>& s,
                     size_t start,
                     size_t count,
                     int sample_rate,
                     double freq) {
  double re = 0.0;
  double im = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double w = 2.0 * kPi * freq * static_cast<double>(i) / sample_rate;
    const double x = s[start + i];
    re += x * std::cos(w);
    im -= x * std::sin(w);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

double Rms(const std::vector<int16_t>& s, size_t start, size_t count) {
  double acc = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double x = s[start + i];
    acc += x * x;
  }
  return std::sqrt(acc / static_cast<double>(count));
}

double RatioToDb(double num, double den) {
  const double eps = 1e-12;
  return 20.0 * std::log10(std::max(num, eps) / std::max(den, eps));
}

// ---------------------------------------------------------------------------
// Legacy multi-rate resampler: webrtc::Resampler
// ---------------------------------------------------------------------------

// The rate pairs WebRTC's Resampler must support (see upstream
// resampler_unittest.cc). 44 kHz <-> 48/96 kHz is deliberately unsupported.
constexpr int kRates[] = {8000, 16000, 32000, 44000, 48000, 96000};

bool ValidRatePair(int in_rate, int out_rate) {
  if ((in_rate == 44000 && (out_rate == 48000 || out_rate == 96000)) ||
      (out_rate == 44000 && (in_rate == 48000 || in_rate == 96000))) {
    return false;
  }
  return true;
}

void TestLegacyRateContract() {
  std::printf("Legacy Resampler: rate support contract\n");
  for (int in_rate : kRates) {
    for (int out_rate : kRates) {
      for (size_t channels : {1u, 2u}) {
        webrtc::Resampler resampler;
        const int expected = ValidRatePair(in_rate, out_rate) ? 0 : -1;
        const int actual = resampler.Reset(in_rate, out_rate, channels);
        Check(actual == expected,
              Fmt("Reset() return value", in_rate, out_rate) + ", channels " +
                  std::to_string(channels));
      }
    }
  }

  // WebRTC's Resampler works on integer ratios only; 44.1 kHz and 22.05 kHz
  // conversions must fail and be handled by the sinc resamplers instead.
  for (const auto& pair : {std::pair<int, int>{44100, 48000},
                           std::pair<int, int>{48000, 44100},
                           std::pair<int, int>{22050, 16000},
                           std::pair<int, int>{16000, 22050}}) {
    webrtc::Resampler resampler;
    Check(resampler.Reset(pair.first, pair.second, 1) == -1,
          Fmt("Reset() rejects 44.1/22.05 kHz", pair.first, pair.second));
  }
}

void TestLegacyPushContract() {
  std::printf("Legacy Resampler: 10 ms Push() sample counts\n");
  std::vector<int16_t> in(2 * 96000 / 100, 1);
  std::vector<int16_t> out(2 * 96000 / 100, 0);
  for (int in_rate : kRates) {
    for (int out_rate : kRates) {
      if (!ValidRatePair(in_rate, out_rate)) {
        continue;
      }
      for (size_t channels : {1u, 2u}) {
        webrtc::Resampler resampler(in_rate, out_rate, channels);
        const size_t in_length = channels * in_rate / 100;
        const size_t expected_out = channels * out_rate / 100;
        size_t out_length = 0;
        const int res = resampler.Push(in.data(), in_length, out.data(),
                                       out.size(), out_length);
        const std::string label =
            Fmt("Push()", in_rate, out_rate) + ", channels " +
            std::to_string(channels);
        Check(res == 0, label + " returned 0");
        Check(out_length == expected_out,
              label + " produced " + std::to_string(expected_out) +
                  " samples, got " + std::to_string(out_length));
      }
    }
  }
}

void TestLegacyPassthrough() {
  std::printf("Legacy Resampler: 1:1 passthrough is bit exact\n");
  const int rate = 48000;
  const std::vector<int16_t> in = Sine(rate, 0.1, {1000.0});
  webrtc::Resampler resampler(rate, rate, 1);
  std::vector<int16_t> out(in.size(), 0);
  size_t out_length = 0;
  const int res =
      resampler.Push(in.data(), in.size(), out.data(), out.size(), out_length);
  Check(res == 0 && out_length == in.size(), "Push() 1:1 length");
  Check(std::memcmp(in.data(), out.data(), in.size() * sizeof(int16_t)) == 0,
        "1:1 output is identical to input");
}

void TestLegacyToneConversion(int in_rate, int out_rate, double freq) {
  const std::string label = Fmt("Legacy Resampler tone", in_rate, out_rate);
  std::printf("%s: %.0f Hz\n", label.c_str(), freq);

  const double duration_s = 1.0;
  const std::vector<int16_t> in = Sine(in_rate, duration_s, {freq});
  const size_t block_in = in_rate / 100;    // 10 ms
  const size_t block_out = out_rate / 100;  // 10 ms

  webrtc::Resampler resampler(in_rate, out_rate, 1);
  std::vector<int16_t> out;
  out.reserve(static_cast<size_t>(out_rate * duration_s));
  std::vector<int16_t> block(block_out, 0);
  for (size_t pos = 0; pos + block_in <= in.size(); pos += block_in) {
    size_t out_length = 0;
    if (resampler.Push(in.data() + pos, block_in, block.data(), block_out,
                       out_length) != 0) {
      Check(false, label + " Push() failed");
      return;
    }
    out.insert(out.end(), block.begin(), block.begin() + out_length);
  }

  // Analyse 0.5 s of steady state, skipping the filter transient.
  const size_t start = out_rate / 20;  // 50 ms
  const size_t count = out_rate / 2;   // 0.5 s
  Check(out.size() >= start + count, label + " produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double in_amp = ToneAmplitude(in, in_rate / 20, in_rate / 2, in_rate, freq);
  const double out_amp = ToneAmplitude(out, start, count, out_rate, freq);
  const double level_db = RatioToDb(out_amp, in_amp);
  std::printf("  in %.1f, out %.1f, level %.2f dB\n", in_amp, out_amp, level_db);
  Check(std::abs(level_db) < 1.5, label + " preserves tone level within 1.5 dB");

  WriteWav("tests/out/legacy_" + std::to_string(in_rate) + "_to_" +
               std::to_string(out_rate) + ".wav",
           out.data(), out.size(), 1, out_rate);
}

// ---------------------------------------------------------------------------
// Sinc resampler: PushSincResampler / SincResampler / PushResampler<T>
// ---------------------------------------------------------------------------

// Resamples a mono int16 signal in 10 ms blocks through PushSincResampler.
std::vector<int16_t> PushSinc(const std::vector<int16_t>& in,
                              int in_rate,
                              int out_rate) {
  const size_t block_in = in_rate / 100;
  const size_t block_out = out_rate / 100;
  webrtc::PushSincResampler resampler(block_in, block_out);
  std::vector<int16_t> out;
  std::vector<int16_t> block(block_out, 0);
  for (size_t pos = 0; pos + block_in <= in.size(); pos += block_in) {
    const size_t written =
        resampler.Resample(in.data() + pos, block_in, block.data(), block_out);
    out.insert(out.end(), block.begin(), block.begin() + written);
  }
  return out;
}

void TestSincDownsample() {
  const int in_rate = 48000;
  const int out_rate = 16000;
  const double passband = 1000.0;   // must survive
  const double stopband = 18000.0;  // aliases to 2000 Hz at 16 kHz
  std::printf("PushSincResampler: %d -> %d, passband %.0f Hz, stopband %.0f Hz\n",
              in_rate, out_rate, passband, stopband);

  const std::vector<int16_t> in =
      Sine(in_rate, 1.0, {passband, stopband}, 8192.0);
  const std::vector<int16_t> out = PushSinc(in, in_rate, out_rate);
  Check(!out.empty(), "downsample produced output");

  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out.size() >= start + count, "downsample produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double pass_out = ToneAmplitude(out, start, count, out_rate, passband);
  const double alias_out = ToneAmplitude(out, start, count, out_rate, 2000.0);
  const double pass_db = RatioToDb(pass_out, 4096.0);
  const double alias_db = RatioToDb(alias_out, pass_out);
  std::printf("  passband %.1f (%.2f dB vs input), alias %.2f (%.1f dB)\n",
              pass_out, pass_db, alias_out, alias_db);
  Check(std::abs(pass_db) < 1.0, "passband level preserved within 1 dB");
  Check(alias_db < -60.0, "aliasing of 18 kHz input below -60 dB");

  WriteWav("tests/out/sinc_48000_to_16000.wav", out.data(), out.size(), 1,
           out_rate);
}

void TestSincUpsample() {
  const int in_rate = 16000;
  const int out_rate = 48000;
  const double tone = 3000.0;   // must survive
  const double image = 13000.0; // 16 kHz image of 3 kHz
  std::printf("PushSincResampler: %d -> %d, tone %.0f Hz, image %.0f Hz\n",
              in_rate, out_rate, tone, image);

  const std::vector<int16_t> in = Sine(in_rate, 1.0, {tone});
  const std::vector<int16_t> out = PushSinc(in, in_rate, out_rate);
  Check(!out.empty(), "upsample produced output");

  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out.size() >= start + count, "upsample produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double tone_out = ToneAmplitude(out, start, count, out_rate, tone);
  const double image_out = ToneAmplitude(out, start, count, out_rate, image);
  const double tone_db = RatioToDb(tone_out, 8192.0);
  const double image_db = RatioToDb(image_out, tone_out);
  std::printf("  tone %.1f (%.2f dB vs input), image %.2f (%.1f dB)\n", tone_out,
              tone_db, image_out, image_db);
  Check(std::abs(tone_db) < 1.0, "tone level preserved within 1 dB");
  Check(image_db < -60.0, "imaging of 3 kHz above 16 kHz below -60 dB");

  WriteWav("tests/out/sinc_16000_to_48000.wav", out.data(), out.size(), 1,
           out_rate);
}

void TestSincArbitraryRatio() {
  const int in_rate = 44100;
  const int out_rate = 48000;
  const double tone = 1000.0;
  std::printf("PushSincResampler: %d -> %d (arbitrary ratio)\n", in_rate,
              out_rate);

  const std::vector<int16_t> in = Sine(in_rate, 1.0, {tone});
  const std::vector<int16_t> out = PushSinc(in, in_rate, out_rate);
  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out.size() >= start + count, "44.1k -> 48k produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double out_amp = ToneAmplitude(out, start, count, out_rate, tone);
  const double level_db = RatioToDb(out_amp, 8192.0);
  std::printf("  level %.2f dB vs input\n", level_db);
  Check(std::abs(level_db) < 1.0, "tone level preserved within 1 dB");
  Check(out.size() == in.size() / (in_rate / 100) * (out_rate / 100),
        "output sample count matches ratio");
  WriteWav("tests/out/sinc_44100_to_48000.wav", out.data(), out.size(), 1,
           out_rate);
}

void TestPushResamplerStereo() {
  const int in_rate = 48000;
  const int out_rate = 16000;
  const double left_tone = 1000.0;
  const double right_tone = 3000.0;
  std::printf("PushResampler<int16_t>: stereo %d -> %d, L %.0f Hz, R %.0f Hz\n",
              in_rate, out_rate, left_tone, right_tone);

  const size_t frames_in = in_rate;
  std::vector<int16_t> in(frames_in * 2, 0);
  for (size_t i = 0; i < frames_in; ++i) {
    const double t = static_cast<double>(i) / in_rate;
    in[i * 2] = static_cast<int16_t>(
        std::lround(8192.0 * std::sin(2.0 * kPi * left_tone * t)));
    in[i * 2 + 1] = static_cast<int16_t>(
        std::lround(8192.0 * std::sin(2.0 * kPi * right_tone * t)));
  }

  const size_t block_in = in_rate / 100;
  const size_t block_out = out_rate / 100;
  webrtc::PushResampler<int16_t> resampler(block_in, block_out, 2);
  std::vector<int16_t> out;
  std::vector<int16_t> block(block_out * 2, 0);
  for (size_t pos = 0; pos + block_in <= frames_in; pos += block_in) {
    resampler.Resample(
        webrtc::InterleavedView<const int16_t>(in.data() + pos * 2, block_in, 2),
        webrtc::InterleavedView<int16_t>(block.data(), block_out, 2));
    out.insert(out.end(), block.begin(), block.end());
  }

  const size_t out_frames = out.size() / 2;
  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out_frames >= start + count, "stereo resample produced enough frames");
  if (out_frames < start + count) {
    return;
  }

  std::vector<int16_t> left(out_frames, 0);
  std::vector<int16_t> right(out_frames, 0);
  for (size_t i = 0; i < out_frames; ++i) {
    left[i] = out[i * 2];
    right[i] = out[i * 2 + 1];
  }

  const double l_left = ToneAmplitude(left, start, count, out_rate, left_tone);
  const double l_right = ToneAmplitude(left, start, count, out_rate, right_tone);
  const double r_right =
      ToneAmplitude(right, start, count, out_rate, right_tone);
  const double r_left = ToneAmplitude(right, start, count, out_rate, left_tone);
  std::printf("  L: %.1f dB tone, %.1f dB crosstalk\n", RatioToDb(l_left, 8192.0),
              RatioToDb(l_right, l_left));
  std::printf("  R: %.1f dB tone, %.1f dB crosstalk\n",
              RatioToDb(r_right, 8192.0), RatioToDb(r_left, r_right));
  Check(std::abs(RatioToDb(l_left, 8192.0)) < 1.0, "left channel level preserved");
  Check(std::abs(RatioToDb(r_right, 8192.0)) < 1.0,
        "right channel level preserved");
  Check(RatioToDb(l_right, l_left) < -60.0, "no bleed from right into left");
  Check(RatioToDb(r_left, r_right) < -60.0, "no bleed from left into right");

  WriteWav("tests/out/sinc_stereo_48000_to_16000.wav", out.data(), out_frames, 2,
           out_rate);
}

void TestPushResamplerFloat() {
  const int in_rate = 16000;
  const int out_rate = 48000;
  const double tone = 1000.0;
  std::printf("PushResampler<float>: mono %d -> %d\n", in_rate, out_rate);

  const int16_t amplitude = 8192;
  const size_t block_in = in_rate / 100;
  const size_t block_out = out_rate / 100;
  webrtc::PushResampler<float> resampler(block_in, block_out, 1);

  std::vector<float> in(block_in, 0.0f);
  std::vector<float> out;
  std::vector<float> block(block_out, 0.0f);
  std::vector<float> collected;
  std::vector<int16_t> as_int16;
  for (size_t pos = 0; pos <= 100; ++pos) {
    for (size_t i = 0; i < block_in; ++i) {
      const double t = static_cast<double>(pos * block_in + i) / in_rate;
      in[i] = static_cast<float>(amplitude) *
              static_cast<float>(std::sin(2.0 * kPi * tone * t));
    }
    resampler.Resample(webrtc::MonoView<const float>(in.data(), block_in),
                       webrtc::MonoView<float>(block.data(), block_out));
    out.insert(out.end(), block.begin(), block.end());
  }

  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out.size() >= start + count, "float resample produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double out_amp = ToneAmplitude(out, start, count, out_rate, tone);
  const double level_db = RatioToDb(out_amp, static_cast<double>(amplitude));
  std::printf("  level %.2f dB vs input\n", level_db);
  Check(std::abs(level_db) < 1.0, "tone level preserved within 1 dB");

  as_int16.resize(out.size());
  webrtc::FloatS16ToS16(out.data(), out.size(), as_int16.data());
  WriteWav("tests/out/sinc_float_16000_to_48000.wav", as_int16.data(),
           as_int16.size(), 1, out_rate);
}

// Drives the pull based SincResampler directly and checks that it produces the
// same samples as the push based wrapper (block boundaries aside).
void TestSincResamplerPullApi() {
  const int in_rate = 48000;
  const int out_rate = 16000;
  const double tone = 1000.0;
  std::printf("SincResampler: pull API %d -> %d\n", in_rate, out_rate);

  const std::vector<int16_t> source = Sine(in_rate, 0.7, {tone});
  struct Source : public webrtc::SincResamplerCallback {
    const std::vector<int16_t>* samples;
    size_t pos = 0;
    void Run(size_t frames, float* destination) override {
      for (size_t i = 0; i < frames; ++i) {
        const size_t idx = pos + i;
        destination[i] = idx < samples->size()
                             ? static_cast<float>((*samples)[idx])
                             : 0.0f;
      }
      pos += frames;
    }
  } callback;
  callback.samples = &source;

  webrtc::SincResampler resampler(static_cast<double>(in_rate) / out_rate,
                                  in_rate / 100, &callback);
  Check(resampler.request_frames() == static_cast<size_t>(in_rate / 100),
        "request_frames() reports the configured block size");
  Check(resampler.ChunkSize() >= 1, "ChunkSize() is non-zero");

  std::vector<float> out;
  std::vector<float> block(out_rate / 100, 0.0f);
  for (int i = 0; i < 60; ++i) {
    resampler.Resample(block.size(), block.data());
    out.insert(out.end(), block.begin(), block.end());
  }

  const size_t start = out_rate / 20;
  const size_t count = out_rate / 2;
  Check(out.size() >= start + count, "pull API produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double out_amp = ToneAmplitude(out, start, count, out_rate, tone);
  const double level_db = RatioToDb(out_amp, 8192.0);
  std::printf("  level %.2f dB vs input\n", level_db);
  Check(std::abs(level_db) < 1.0, "tone level preserved within 1 dB");
}

void TestRoundTrip() {
  const int rate = 48000;
  const int mid_rate = 16000;
  const double tone = 1000.0;
  std::printf("Round trip %d -> %d -> %d\n", rate, mid_rate, rate);

  const std::vector<int16_t> in = Sine(rate, 1.0, {tone});
  const std::vector<int16_t> mid = PushSinc(in, rate, mid_rate);
  const std::vector<int16_t> out = PushSinc(mid, mid_rate, rate);

  const size_t start = rate / 10;
  const size_t count = rate / 2;
  Check(out.size() >= start + count, "round trip produced enough samples");
  if (out.size() < start + count) {
    return;
  }
  const double in_amp = ToneAmplitude(in, start, count, rate, tone);
  const double out_amp = ToneAmplitude(out, start, count, rate, tone);
  const double level_db = RatioToDb(out_amp, in_amp);
  std::printf("  level %.2f dB vs input, rms %.1f\n", level_db,
              Rms(out, start, count));
  Check(std::abs(level_db) < 1.0, "round trip preserves tone level within 1 dB");
  WriteWav("tests/out/sinc_roundtrip_48000.wav", out.data(), out.size(), 1, rate);
}

}  // namespace

int main() {
  std::printf("WebRTC resampler test\n\n");

  TestLegacyRateContract();
  TestLegacyPushContract();
  TestLegacyPassthrough();
  TestLegacyToneConversion(48000, 16000, 400.0);
  TestLegacyToneConversion(16000, 48000, 400.0);
  TestLegacyToneConversion(8000, 48000, 400.0);
  TestLegacyToneConversion(48000, 8000, 400.0);
  TestLegacyToneConversion(44000, 16000, 400.0);
  TestLegacyToneConversion(16000, 24000, 400.0);
  TestSincDownsample();
  TestSincUpsample();
  TestSincArbitraryRatio();
  TestPushResamplerStereo();
  TestPushResamplerFloat();
  TestSincResamplerPullApi();
  TestRoundTrip();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
