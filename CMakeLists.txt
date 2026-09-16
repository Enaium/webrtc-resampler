cmake_minimum_required(VERSION 3.16)

set(PROJECT webrtc_resampler)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

project(${PROJECT} VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W3 /EHsc)
else()
    add_compile_options(-Wall -fPIC -Wno-deprecated -fexceptions)
endif()

if(APPLE)
    add_definitions(-DWEBRTC_MAC -DWEBRTC_POSIX)
elseif(UNIX AND NOT APPLE)
    add_definitions(-DWEBRTC_LINUX -DWEBRTC_POSIX)
elseif(WIN32)
    add_definitions(-DWEBRTC_WIN)
endif()

# Collect all source files
file(GLOB_RECURSE RESAMPLER_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/**/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/**/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/**/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/**/*.h
)

# The SIMD units are compiled per architecture, mirroring WebRTC's build
# configuration (see webrtc.gni and common_audio/BUILD.gn).
list(FILTER RESAMPLER_SRC EXCLUDE REGEX ".*sinc_resampler_(sse|avx2|neon)\\.cc$")

set(SSE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/common_audio/resampler/sinc_resampler_sse.cc)
set(AVX2_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/common_audio/resampler/sinc_resampler_avx2.cc)
set(NEON_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/common_audio/resampler/sinc_resampler_neon.cc)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|x86|i686")
    list(APPEND RESAMPLER_SRC ${SSE_SRC})
    if(MSVC)
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set_source_files_properties(${SSE_SRC} PROPERTIES COMPILE_OPTIONS "/arch:SSE2")
        endif()
    else()
        set_source_files_properties(${SSE_SRC} PROPERTIES COMPILE_OPTIONS "-msse2")
    endif()

    # WebRTC enables the AVX2 runtime dispatch for Clang builds only.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_definitions(-DWEBRTC_ENABLE_AVX2)
        list(APPEND RESAMPLER_SRC ${AVX2_SRC})
        if(MSVC)
            set_source_files_properties(${AVX2_SRC} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        else()
            set_source_files_properties(${AVX2_SRC} PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
        endif()
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm|ARM")
    add_definitions(-DWEBRTC_HAS_NEON)
    list(APPEND RESAMPLER_SRC ${NEON_SRC})
endif()

add_library(${PROJECT} SHARED ${RESAMPLER_SRC})

target_include_directories(${PROJECT} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(${PROJECT} PROPERTIES LINKER_LANGUAGE CXX)

option(BUILD_TESTS "Build the resampler test executable" OFF)

if(BUILD_TESTS)
    add_executable(resampler_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/resampler_test.cc
    )

    target_link_libraries(resampler_test PRIVATE ${PROJECT})

    target_include_directories(resampler_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()
