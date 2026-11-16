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
# All JUCE modules, compiled once into one static library shared by the engine,
# the app, the CLI host and the tests (avoids compiling juce_core twice).
# ---------------------------------------------------------------------------
add_library(hype_juce STATIC)

target_compile_definitions(hype_juce
    PUBLIC
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=1
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_APPLICATION_NAME_STRING="HYPE DJ PRO"
        JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(hype_juce
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_data_structures
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ASIO (Windows): needs the Steinberg ASIO SDK, which can't be redistributed.
set(HYPE_ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (enables ASIO on Windows)")
if(WIN32 AND HYPE_ASIO_SDK_DIR)
    target_compile_definitions(hype_juce PUBLIC JUCE_ASIO=1)
    target_include_directories(hype_juce PRIVATE "${HYPE_ASIO_SDK_DIR}/common")
    message(STATUS "HYPE: ASIO enabled (${HYPE_ASIO_SDK_DIR})")
endif()

# JACK (Linux), if the headers are installed.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(JACK QUIET jack)
        if(JACK_FOUND)
            target_compile_definitions(hype_juce PUBLIC JUCE_JACK=1)
            target_include_directories(hype_juce PRIVATE ${JACK_INCLUDE_DIRS})
        endif()
    endif()
endif()

# Re-export the JUCE include paths and definitions to consumers.
target_include_directories(hype_juce INTERFACE $<TARGET_PROPERTY:hype_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(hype_juce INTERFACE $<TARGET_PROPERTY:hype_juce,COMPILE_DEFINITIONS>)

# ---------------------------------------------------------------------------
# Core engine library
# ---------------------------------------------------------------------------
add_library(hype_engine STATIC
    Source/Engine/AudioDeviceController.cpp
    Source/Engine/AudioDeviceController.h
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
    Source/Engine/TimeStretcher.cpp
    Source/Engine/TimeStretcher.h
    Source/Engine/WaveformData.cpp
    Source/Engine/WaveformData.h)

target_include_directories(hype_engine PUBLIC Source)
target_compile_definitions(hype_engine PUBLIC HYPE_HAS_FFMPEG=${HYPE_HAS_FFMPEG})
target_link_libraries(hype_engine PUBLIC hype_juce PRIVATE signalsmith-stretch)

if(HYPE_HAS_FFMPEG)
    target_link_libraries(hype_engine PRIVATE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# Desktop application
# ---------------------------------------------------------------------------
option(HYPE_BUILD_APP "Build the HYPE DJ PRO desktop application" ON)
if(HYPE_BUILD_APP)
    juce_add_gui_app(HypeDJPro
        PRODUCT_NAME "HYPE DJ PRO"
        COMPANY_NAME "HYPE"
        BUNDLE_ID    "com.hype.djpro"
        MICROPHONE_PERMISSION_ENABLED FALSE)

    target_sources(HypeDJPro PRIVATE
        Source/App/AudioSettingsComponent.cpp
        Source/App/AudioSettingsComponent.h
        Source/App/DeckComponent.cpp
        Source/App/DeckComponent.h
        Source/App/HypeLookAndFeel.cpp
        Source/App/HypeLookAndFeel.h
        Source/App/JogWheel.cpp
        Source/App/JogWheel.h
        Source/App/Main.cpp
        Source/App/MainComponent.cpp
        Source/App/MainComponent.h
        Source/App/MixerComponent.cpp
        Source/App/MixerComponent.h
        Source/App/WaveformViews.cpp
        Source/App/WaveformViews.h
        Source/App/UiSelfTest.cpp
        Source/App/UiSelfTest.h
        Source/App/Widgets.cpp
        Source/App/Widgets.h)

    target_link_libraries(HypeDJPro PRIVATE hype_engine)
endif()

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
