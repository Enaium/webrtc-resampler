# WebRTC Resampler

A standalone extraction of the WebRTC resampler, taken from
[`common_audio/resampler`](https://webrtc.googlesource.com/src/+/refs/heads/main/common_audio/resampler)
and the signal processing subset it depends on.

Extracted from WebRTC revision `c25c6676c6` (2026-11-18). Every extracted file
under `common_audio/` is byte identical to upstream; only `api/audio/audio_view.h`
is adapted (see [Notes](#notes)).

## Contents

| Component | Header | Purpose |
| --- | --- | --- |
| `webrtc::Resampler` | `common_audio/resampler/include/resampler.h` | Integer multi-rate resampler (`int16_t`), based on the signal processing library |
| `webrtc::SincResampler` | `common_audio/resampler/sinc_resampler.h` | High quality single channel windowed sinc resampler (pull interface) |
| `webrtc::PushSincResampler` | `common_audio/resampler/push_sinc_resampler.h` | Push interface wrapper around `SincResampler` |
| `webrtc::PushResampler<T>` | `common_audio/resampler/include/push_resampler.h` | Multichannel wrapper over `PushSincResampler` (`int16_t` and `float`) |
| Buffer views | `api/audio/audio_view.h` | `MonoView<T>`, `InterleavedView<T>`, `DeinterleavedView<T>` used by the push APIs |

`webrtc::Resampler` supports the fixed rate pairs WebRTC supports (1:1, 1:2,
1:3, 1:4, 1:6, 1:12, 2:3, 2:11, 4:11, 8:11, 11:16, 11:32 and the inverses -
e.g. 8/16/32/44/48/96 kHz combinations, but not 44 kHz <-> 48/96 kHz and, like
WebRTC, neither 44.1 kHz nor 22.05 kHz ratios).
`SincResampler`/`PushSincResampler`/`PushResampler` handle arbitrary ratios
(44.1 kHz <-> 48 kHz, etc.) at a higher quality and cost.

## Build

This project uses [pixi](https://pixi.sh) with CMake and Ninja.

```bash
pixi run -e build configure
pixi run -e build build
```

This produces `libwebrtc_resampler.dylib` (macOS), `libwebrtc_resampler.so`
(Linux), or `webrtc_resampler.dll` (Windows) in `.build/`. The library has no
dependencies beyond the C++ standard library.

## Test

```bash
pixi run -e build configure-test
pixi run -e build build-test
pixi run -e build run-test
```

The test drives both resamplers (rate support contract, 10 ms block sample
counts, 1:1 bit exact passthrough, tone level preservation, aliasing/imaging
rejection, stereo channel separation, pull vs push API) and writes the processed
signals to `tests/out/*.wav`.

## Usage

```cpp
#include "common_audio/resampler/push_sinc_resampler.h"

// 10 ms block at 48 kHz -> 10 ms block at 16 kHz.
constexpr size_t kSourceFrames = 480;
constexpr size_t kDestinationFrames = 160;

webrtc::PushSincResampler resampler(kSourceFrames, kDestinationFrames);
size_t written = resampler.Resample(source, kSourceFrames,
                                    destination, kDestinationFrames);
```

```cpp
#include "common_audio/resampler/include/resampler.h"

// Integer multi-rate resampler, 48 kHz -> 16 kHz, mono.
webrtc::Resampler resampler(48000, 16000, 1);
size_t out_length = 0;
resampler.Push(samples_in, 480, samples_out, 160, out_length);
```

## Notes

* **SIMD**: NEON is used on arm64, SSE2 on x86, and AVX2 is selected at runtime
  on x86 when built with Clang (`-DWEBRTC_ENABLE_AVX2`), matching WebRTC's build
  configuration. Other architectures fall back to the C implementation.
* **Support layer**: the resampler includes WebRTC's `rtc_base` helpers
  (`checks`, `logging`, `cpu_info`, `aligned_malloc`, ...). They are vendored
  from the same revision that [webrtc-aec3](https://github.com/Enaium/webrtc-aec3)
  uses, together with small `absl` header shims
  (`absl::string_view`/`optional`/`type_traits` aliasing `std`). Note that those
  helpers live in the `rtc` namespace, while the resampler itself is in `webrtc`.
* **Only upstream edit**: `api/audio/audio_view.h` uses `std::fill` instead of
  `absl::c_fill` (= `absl/algorithm/container.h`), so that no Abseil
  implementation is needed.
* **Signal processing subset**: only the files required by `webrtc::Resampler`
  are extracted (`resample*.c`, `resample_by_2*`, `spl_inl*`,
  `spl_sqrt_floor`). The NEON/SSE/MIPS dispatch table `spl_init.c` and the
  functions it dispatches to (cross correlation, min/max, downsample fast) are
  not part of this extraction.
* **Toolchain**: the build uses the system compiler instead of conda-forge's
  `cxx-compiler`; the linker bundled with that toolchain (`ld64` 956.6) cannot
  read the `.tbd` stubs of the current macOS SDK.

## License

BSD 3-Clause (same as WebRTC). See [LICENSE](LICENSE).
`common_audio/third_party/spl_sqrt_floor` is licensed separately, see
[its LICENSE](src/common_audio/third_party/spl_sqrt_floor/LICENSE).
