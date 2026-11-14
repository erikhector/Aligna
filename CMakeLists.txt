cmake_minimum_required(VERSION 3.22)
# Must be set before project() so every target (including FetchContent'd JUCE and Catch2) sees them.
# Windows: static MSVC runtime so the release .exe does not need the VC++ redistributable (CMP0091 is NEW at 3.22).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# macOS: universal binary, deployment target 11.0 (the first release with arm64). Cache entries so -D can still override.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
project(Aligna VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)
# Dependencies are pinned to commit hashes (a tag can be moved); the comment names the release each one is.
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 51d11a2be6d5c97ccf12b4e5e827006e19f0555a # 8.0.4
  GIT_SHALLOW ON)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14 # v3.7.1
  GIT_SHALLOW ON)
# Rubber Band (GPL-2.0-or-later): no CMake build, we compile its single-file unit into aligna_core.
FetchContent_Declare(rubberband
  GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
  GIT_TAG 1d95888bec3ae0a17c0c4af791810d5a63f6bc35 # v4.0.0
  GIT_SHALLOW ON)
# LAME 3.100 (LGPL): MP3 encoder, only libmp3lame/*.c is built (see lame_enc below).
FetchContent_Declare(lame
  URL https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(JUCE Catch2 rubberband lame)

# Compile definitions shared by every target so app and tests see the same JUCE config.
set(ALIGNA_JUCE_DEFS
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_APPLICATION_NAME_STRING="Aligna"
  JUCE_APPLICATION_VERSION_STRING="0.2.1")

# ---------------------------------------------------------------------------
# aligna_core: static library. Later tasks append files to the target_sources
# list below. JUCE modules are linked PUBLIC here; the app and tests link only
# aligna_core (the app additionally links juce_gui_basics / juce_gui_extra).
# ---------------------------------------------------------------------------
add_library(aligna_core STATIC)
target_sources(aligna_core PRIVATE
  src/core/Version.cpp
  src/edit/ClipOps.cpp
  src/edit/DragMath.cpp
  src/edit/StretchMath.cpp
  src/edit/RecordTiming.cpp
  src/edit/EditHistory.cpp
  src/io/ProjectFile.cpp
  src/io/FileNaming.cpp
  src/io/TakeFiles.cpp
  src/io/Mp3Encoder.cpp
  src/engine/Mixer.cpp
  src/engine/PlaybackChain.cpp
  src/engine/PlaybackEngine.cpp
  src/engine/Recorder.cpp
  src/engine/SourcePool.cpp
  src/engine/Stretcher.cpp
  src/engine/StretchCache.cpp
  src/model/Grid.cpp
  src/align/Correlate.cpp
  src/align/AlignClips.cpp
  src/align/AlignResults.cpp
  src/ui/TimelineMapping.cpp
)
# Third-party Rubber Band unit: built-in FFT/resampler (vDSP on Apple), warnings silenced for this file only.
set(RB_SINGLE ${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp)
target_sources(aligna_core PRIVATE ${RB_SINGLE})
if(MSVC)
  set_source_files_properties(${RB_SINGLE} PROPERTIES COMPILE_OPTIONS "/w" COMPILE_DEFINITIONS "NOMINMAX")
else()
  set_source_files_properties(${RB_SINGLE} PROPERTIES COMPILE_OPTIONS "-w")
endif()
# LAME encoder library (no mpglib, no frontend, no SIMD quantize helper), warnings silenced for this target only.
file(GLOB LAME_SOURCES CONFIGURE_DEPENDS ${lame_SOURCE_DIR}/libmp3lame/*.c)
add_library(lame_enc STATIC ${LAME_SOURCES})
target_include_directories(lame_enc
  PUBLIC ${lame_SOURCE_DIR}/include
  PRIVATE ${lame_SOURCE_DIR}/libmp3lame ${lame_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lame)
target_compile_definitions(lame_enc PRIVATE HAVE_CONFIG_H STDC_HEADERS HAVE_STDINT_H HAVE_ERRNO_H
  HAVE_FCNTL_H HAVE_LIMITS_H ieee754_float32_t=float)
if(MSVC)
  target_compile_definitions(lame_enc PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(lame_enc PRIVATE /w)
else()
  target_compile_definitions(lame_enc PRIVATE TAKEHIRO_IEEE754_HACK)
  target_compile_options(lame_enc PRIVATE -w)
endif()
set_target_properties(lame_enc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# License texts for release packaging: copied next to the app so CI needn't know _deps paths.
configure_file(${lame_SOURCE_DIR}/COPYING ${CMAKE_BINARY_DIR}/LICENSE-LAME.txt COPYONLY)
configure_file(${rubberband_SOURCE_DIR}/COPYING ${CMAKE_BINARY_DIR}/LICENSE-RubberBand.txt COPYONLY)

target_include_directories(aligna_core PUBLIC src)
target_include_directories(aligna_core PRIVATE ${rubberband_SOURCE_DIR})
if(APPLE)
  target_link_libraries(aligna_core PUBLIC "-framework Accelerate")
endif()
target_compile_features(aligna_core PUBLIC cxx_std_20)
target_compile_definitions(aligna_core PUBLIC ${ALIGNA_JUCE_DEFS})
target_link_libraries(aligna_core
  PRIVATE lame_enc
  PUBLIC
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Aligna app
# ---------------------------------------------------------------------------
juce_add_gui_app(Aligna
  PRODUCT_NAME "Aligna"
  COMPANY_NAME "Aligna"
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "Aligna uses audio input to record takes."
  VERSION ${PROJECT_VERSION})
target_sources(Aligna PRIVATE
  src/app/Main.cpp
  src/app/AppController.cpp
  src/app/AppSettings.cpp
  src/ui/InputMeter.cpp
  src/ui/AudioSettingsPanel.cpp
  src/ui/TransportBar.cpp
  src/ui/TrackHeader.cpp
  src/ui/TimelineView.cpp
  src/ui/AlignDialog.cpp
  src/ui/ClipTempoDialog.cpp
  src/ui/MainComponent.cpp
)
# Microphone permission: AVFoundation on macOS, a trivial always-granted version elsewhere.
if(APPLE)
  target_sources(Aligna PRIVATE src/app/MicPermission_mac.mm)
  target_link_libraries(Aligna PRIVATE "-framework AVFoundation")
else()
  target_sources(Aligna PRIVATE src/app/MicPermission.cpp)
endif()
target_compile_definitions(Aligna PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(Aligna
  PRIVATE
    aligna_core
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_recommended_lto_flags)

# ---------------------------------------------------------------------------
# Tests (Catch2) - link only aligna_core
# ---------------------------------------------------------------------------
enable_testing()
add_executable(aligna_tests
  tests/SmokeTest.cpp
  tests/ClipOpsTest.cpp
  tests/EditHistoryTest.cpp
  tests/GridTest.cpp
  tests/RecordTimingTest.cpp
  tests/ProjectFileTest.cpp
  tests/MixerTest.cpp
  tests/SourcePoolTest.cpp
  tests/PlaybackEngineTest.cpp
  tests/PlaybackChainTest.cpp
  tests/RecorderTest.cpp
  tests/RecordLoopbackTest.cpp
  tests/TimelineMappingTest.cpp
  tests/DragMathTest.cpp
  tests/AlignTest.cpp
  tests/AlignResultsTest.cpp
  tests/FileNamingTest.cpp
  tests/TakeFilesTest.cpp
  tests/StretcherTest.cpp
  tests/StretchOpsTest.cpp
  tests/StretchCacheTest.cpp
  tests/StretchMathTest.cpp
  tests/Mp3EncoderTest.cpp
)
target_link_libraries(aligna_tests PRIVATE aligna_core Catch2::Catch2WithMain)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(aligna_tests)
