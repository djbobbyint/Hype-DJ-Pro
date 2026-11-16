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
set(HYPE_FFMPEG_ROOT "" CACHE PATH "FFmpeg install prefix (include/, lib/, bin/) - used on Windows, where pkg-config isn't available")
if(HYPE_ENABLE_FFMPEG)
    if(HYPE_FFMPEG_ROOT)
        add_library(hype_ffmpeg INTERFACE)
        target_include_directories(hype_ffmpeg INTERFACE "${HYPE_FFMPEG_ROOT}/include")
        foreach(lib avformat avcodec swresample avutil)
            find_library(HYPE_FFMPEG_${lib} NAMES ${lib} lib${lib} lib${lib}.dll.a
                         PATHS "${HYPE_FFMPEG_ROOT}/lib" "${HYPE_FFMPEG_ROOT}/bin"
                         NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
            if(NOT HYPE_FFMPEG_${lib})
                message(FATAL_ERROR "HYPE: ${lib} not found in ${HYPE_FFMPEG_ROOT}")
            endif()
            target_link_libraries(hype_ffmpeg INTERFACE "${HYPE_FFMPEG_${lib}}")
        endforeach()
        file(GLOB HYPE_FFMPEG_RUNTIME "${HYPE_FFMPEG_ROOT}/bin/*.dll")
        set(HYPE_HAS_FFMPEG 1)
        message(STATUS "HYPE: FFmpeg decoder enabled (${HYPE_FFMPEG_ROOT})")
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
            if(FFMPEG_FOUND)
                add_library(hype_ffmpeg INTERFACE)
                target_link_libraries(hype_ffmpeg INTERFACE PkgConfig::FFMPEG)
                set(HYPE_HAS_FFMPEG 1)
                message(STATUS "HYPE: FFmpeg decoder enabled (${FFMPEG_libavformat_VERSION})")
            endif()
        endif()
    endif()
    if(NOT HYPE_HAS_FFMPEG)
        message(STATUS "HYPE: FFmpeg not found - AAC/M4A will only work through the platform decoder (CoreAudio on macOS)")
    endif()
endif()

# Self-contained Windows binaries with MinGW: no libstdc++/libgcc/winpthread DLLs needed.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Copies the FFmpeg DLLs next to a Windows executable.
function(hype_copy_runtime_dlls target)
    if(WIN32 AND HYPE_FFMPEG_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HYPE_FFMPEG_RUNTIME} $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

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

# MinGW-w64 < 12 lacks some Direct2D headers JUCE 8 needs, and Linux file systems are
# case-sensitive (JUCE includes <Dbghelp.h>). scripts/build-windows.sh prepares this directory.
set(HYPE_MINGW_COMPAT_INCLUDE "" CACHE PATH "Extra include directory searched first when building JUCE with MinGW")
if(MINGW AND HYPE_MINGW_COMPAT_INCLUDE)
    target_include_directories(hype_juce BEFORE PRIVATE "${HYPE_MINGW_COMPAT_INCLUDE}")
endif()
if(MINGW)
    # JUCE 8 targets Windows 10 (its Direct2D renderer uses Windows 10 interfaces); MinGW defaults to Windows 7.
    target_compile_definitions(hype_juce PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C)
    # MSVC's <string> pulls in <cstring>; libstdc++'s doesn't, but JUCE relies on it.
    target_compile_options(hype_juce PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-include cstring>)
    # JUCE requests these with #pragma comment(lib), which only MSVC honours.
    target_link_libraries(hype_juce PUBLIC
        d2d1 d3d11 dcomp dxgi dbghelp dwrite dxguid advapi32 comctl32 comdlg32 dwmapi imm32 kernel32
        shlwapi user32 gdi32 version vfw32 wininet winmm ws2_32 ole32 oleaut32 uuid shell32 rpcrt4 setupapi avrt ksuser)
    target_sources(hype_juce PRIVATE cmake/mingw/HypeMingwCompat.cpp)
    # Windows Media Format SDK error code missing from mingw-w64's nserror.h (value from the Windows SDK).
    target_compile_definitions(hype_juce PRIVATE "NS_E_NO_MORE_SAMPLES=((HRESULT)0xC00D0BCFL)")
endif()

# Re-export the JUCE include paths and definitions to consumers.
target_include_directories(hype_juce INTERFACE $<TARGET_PROPERTY:hype_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(hype_juce INTERFACE $<TARGET_PROPERTY:hype_juce,COMPILE_DEFINITIONS>)

# ---------------------------------------------------------------------------
# Core engine library
# ---------------------------------------------------------------------------
add_library(hype_engine STATIC
    Source/Api/HypeEngineApi.h
    Source/Engine/AudioDeviceManager.cpp
    Source/Engine/AudioDeviceManager.h
    Source/Engine/AudioEngine.cpp
    Source/Engine/AudioEngine.h
    Source/Engine/AudioFileDecoder.cpp
    Source/Engine/AudioFileDecoder.h
    Source/Engine/BeatAnalyzer.cpp
    Source/Engine/BeatAnalyzer.h
    Source/Engine/CueEngine.cpp
    Source/Engine/CueEngine.h
    Source/Engine/DeckEngine.cpp
    Source/Engine/DeckEngine.h
    Source/Engine/EngineConfig.h
    Source/Engine/FFmpegDecoder.cpp
    Source/Engine/FFmpegDecoder.h
    Source/Engine/LoadedTrack.h
    Source/Engine/MixerEngine.cpp
    Source/Engine/MixerEngine.h
    Source/Engine/OutputRouter.cpp
    Source/Engine/OutputRouter.h
    Source/Engine/SincInterpolator.cpp
    Source/Engine/SincInterpolator.h
    Source/Engine/SpscQueue.h
    Source/Engine/ThreeBandEq.cpp
    Source/Engine/ThreeBandEq.h
    Source/Engine/TimeStretcher.cpp
    Source/Engine/TimeStretcher.h
    Source/Engine/TrackLoader.cpp
    Source/Engine/TrackLoader.h
    Source/Engine/WaveformData.cpp
    Source/Engine/WaveformData.h)

target_include_directories(hype_engine PUBLIC Source)
target_compile_definitions(hype_engine PUBLIC HYPE_HAS_FFMPEG=${HYPE_HAS_FFMPEG})
target_link_libraries(hype_engine PUBLIC hype_juce PRIVATE signalsmith-stretch)

if(HYPE_HAS_FFMPEG)
    target_link_libraries(hype_engine PRIVATE hype_ffmpeg)
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
        Source/App/SoakRunner.cpp
        Source/App/SoakRunner.h
        Source/App/UiSelfTest.cpp
        Source/App/UiSelfTest.h
        Source/App/Widgets.cpp
        Source/App/Widgets.h)

    target_link_libraries(HypeDJPro PRIVATE hype_engine)
    hype_copy_runtime_dlls(HypeDJPro)

    # The UI may only talk to the engine through Source/Api/HypeEngineApi.h.
    add_custom_target(hype_check_api_boundary
        COMMAND ${CMAKE_COMMAND} -DAPP_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Source/App
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckApiBoundary.cmake
        COMMENT "Checking that the UI only uses the public engine API")
    add_dependencies(HypeDJPro hype_check_api_boundary)
endif()

# ---------------------------------------------------------------------------
# Command-line host
# ---------------------------------------------------------------------------
if(HYPE_BUILD_CLI)
    add_executable(hype_cli Source/Cli/Main.cpp)
    target_link_libraries(hype_cli PRIVATE hype_engine)
    hype_copy_runtime_dlls(hype_cli)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HYPE_BUILD_TESTS)
    enable_testing()
    add_executable(hype_engine_tests Tests/EngineTests.cpp Tests/AllocationTracker.cpp)
    target_link_libraries(hype_engine_tests PRIVATE hype_engine)
    hype_copy_runtime_dlls(hype_engine_tests)
    add_test(NAME hype_engine_tests COMMAND hype_engine_tests)

    # Phase 1 acceptance suite (end-to-end through the public API and a simulated interface)
    add_executable(hype_phase1_tests Tests/Phase1AcceptanceTests.cpp Tests/AllocationTracker.cpp)
    target_link_libraries(hype_phase1_tests PRIVATE hype_engine)
    hype_copy_runtime_dlls(hype_phase1_tests)
    add_test(NAME hype_phase1_tests COMMAND hype_phase1_tests)
    set_tests_properties(hype_phase1_tests PROPERTIES ENVIRONMENT "HYPE_SOAK_MINUTES=30" TIMEOUT 3600)
endif()
