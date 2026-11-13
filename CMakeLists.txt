cmake_minimum_required(VERSION 3.22)
project(Aligna VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW ON)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE Catch2)

# Compile definitions shared by every target so app and tests see the same JUCE config.
set(ALIGNA_JUCE_DEFS
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_APPLICATION_NAME_STRING="Aligna"
  JUCE_APPLICATION_VERSION_STRING="0.1.0")

# ---------------------------------------------------------------------------
# aligna_core: static library. Later tasks append files to the target_sources
# list below. JUCE modules are linked PUBLIC here; the app and tests link only
# aligna_core (the app additionally links juce_gui_basics / juce_gui_extra).
# ---------------------------------------------------------------------------
add_library(aligna_core STATIC)
target_sources(aligna_core PRIVATE
  src/core/Version.cpp
  src/edit/ClipOps.cpp
)
target_include_directories(aligna_core PUBLIC src)
target_compile_features(aligna_core PUBLIC cxx_std_20)
target_compile_definitions(aligna_core PUBLIC ${ALIGNA_JUCE_DEFS})
target_link_libraries(aligna_core
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
  VERSION ${PROJECT_VERSION})
target_sources(Aligna PRIVATE
  src/app/Main.cpp
  src/ui/MainComponent.cpp
)
target_compile_definitions(Aligna PRIVATE JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(Aligna
  PRIVATE
    aligna_core
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_lto_flags)

# ---------------------------------------------------------------------------
# Tests (Catch2) - link only aligna_core
# ---------------------------------------------------------------------------
enable_testing()
add_executable(aligna_tests
  tests/SmokeTest.cpp
  tests/ClipOpsTest.cpp
)
target_link_libraries(aligna_tests PRIVATE aligna_core Catch2::Catch2WithMain)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(aligna_tests)
