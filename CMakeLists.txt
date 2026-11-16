cmake_minimum_required(VERSION 3.24)

project(HypeDJPro VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HYPE_ENABLE_FFMPEG "Use FFmpeg (libavformat/libavcodec) as a fallback decoder for AAC/M4A and other formats JUCE can't read" ON)
option(HYPE_BUILD_TESTS   "Build the engine unit tests" ON)
option(HYPE_BUILD_CLI     "Build the command-line engine host" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout in external/JUCE if present, otherwise fetch it.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Signalsmith Stretch (MIT): phase-vocoder time-stretch / pitch-shift used for
# key-lock and independent pitch.
# ---------------------------------------------------------------------------
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.4.0
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(signalsmith-stretch)
# Treat third-party headers as system headers (no warnings from them).
set_target_properties(signalsmith-stretch signalsmith-linear PROPERTIES SYSTEM TRUE)

# ---------------------------------------------------------------------------
# Optional FFmpeg decoder
# ---------------------------------------------------------------------------
set(HYPE_HAS_FFMPEG 0)
if(HYPE_ENABLE_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
        if(FFMPEG_FOUND)
            set(HYPE_HAS_FFMPEG 1)
            message(STATUS "HYPE: FFmpeg decoder enabled (${FFMPEG_libavformat_VERSION})")
        endif()
    endif()
    if(NOT HYPE_HAS_FFMPEG)
        message(STATUS "HYPE: FFmpeg not found - AAC/M4A will only work through the platform decoder (CoreAudio / Media Foundation)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Core engine library
# ---------------------------------------------------------------------------
add_library(hype_engine STATIC
    Source/Engine/AudioEngine.cpp
    Source/Engine/AudioEngine.h
    Source/Engine/AudioFileDecoder.cpp
    Source/Engine/AudioFileDecoder.h
    Source/Engine/BeatAnalyzer.cpp
    Source/Engine/BeatAnalyzer.h
    Source/Engine/Deck.cpp
    Source/Engine/Deck.h
    Source/Engine/EngineConfig.h
    Source/Engine/FFmpegDecoder.cpp
    Source/Engine/FFmpegDecoder.h
    Source/Engine/LoadedTrack.h
    Source/Engine/Mixer.cpp
    Source/Engine/Mixer.h
    Source/Engine/SincInterpolator.cpp
    Source/Engine/SincInterpolator.h
    Source/Engine/SpscQueue.h
    Source/Engine/ThreeBandEq.cpp
    Source/Engine/ThreeBandEq.h
    Source/Engine/WaveformData.cpp
    Source/Engine/WaveformData.h
    Source/Engine/AudioDeviceController.cpp
    Source/Engine/AudioDeviceController.h
    Source/Engine/TimeStretcher.cpp
    Source/Engine/TimeStretcher.h)

target_include_directories(hype_engine PUBLIC Source)

target_compile_definitions(hype_engine
    PUBLIC
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        HYPE_HAS_FFMPEG=${HYPE_HAS_FFMPEG})

target_link_libraries(hype_engine
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_devices
        signalsmith-stretch
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(HYPE_HAS_FFMPEG)
    target_link_libraries(hype_engine PRIVATE PkgConfig::FFMPEG)
endif()

# JUCE modules are compiled into hype_engine; re-export their include paths and
# definitions so consumers can include JUCE headers without compiling JUCE twice.
target_include_directories(hype_engine INTERFACE
    $<TARGET_PROPERTY:hype_engine,INCLUDE_DIRECTORIES>)
target_compile_definitions(hype_engine INTERFACE
    $<TARGET_PROPERTY:hype_engine,COMPILE_DEFINITIONS>)

# ---------------------------------------------------------------------------
# Command-line host
# ---------------------------------------------------------------------------
if(HYPE_BUILD_CLI)
    add_executable(hype_cli Source/Cli/Main.cpp)
    target_link_libraries(hype_cli PRIVATE hype_engine)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HYPE_BUILD_TESTS)
    enable_testing()
    add_executable(hype_engine_tests Tests/EngineTests.cpp Tests/AllocationTracker.cpp)
    target_link_libraries(hype_engine_tests PRIVATE hype_engine)
    add_test(NAME hype_engine_tests COMMAND hype_engine_tests)
endif()
