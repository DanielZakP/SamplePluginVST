cmake_minimum_required(VERSION 3.22)
project(ChopLab VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(CHOPLAB_BUILD_TESTS "Build the analysis test runner" ON)

# Dependencies: use local checkouts in libs/ if present, otherwise fetch pinned versions.
include(FetchContent)
set(CHOPLAB_LIBS ${CMAKE_CURRENT_SOURCE_DIR}/libs)

if(EXISTS ${CHOPLAB_LIBS}/JUCE/CMakeLists.txt)
    add_subdirectory(${CHOPLAB_LIBS}/JUCE JUCE EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

if(EXISTS ${CHOPLAB_LIBS}/signalsmith-stretch/signalsmith-stretch.h)
    set(SIGNALSMITH_STRETCH_DIR ${CHOPLAB_LIBS}/signalsmith-stretch)
    set(SIGNALSMITH_LINEAR_PARENT ${CHOPLAB_LIBS})
else()
    FetchContent_Declare(signalsmith_stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG 1.4.0
        GIT_SHALLOW ON
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/stretch-parent/signalsmith-stretch
        SOURCE_SUBDIR do-not-add)
    FetchContent_Declare(signalsmith_linear
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
        GIT_TAG 0.6.4
        GIT_SHALLOW ON
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/linear-parent/signalsmith-linear
        SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(signalsmith_stretch signalsmith_linear)
    set(SIGNALSMITH_STRETCH_DIR ${CMAKE_BINARY_DIR}/_deps/stretch-parent/signalsmith-stretch)
    set(SIGNALSMITH_LINEAR_PARENT ${CMAKE_BINARY_DIR}/_deps/linear-parent)
endif()

# Speech-to-text for lyrics. Built static, without OpenMP (no extra DLLs next to the plugin) and
# for a portable x86 baseline (AVX2/FMA) rather than the build machine's CPU.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
if(EXISTS ${CHOPLAB_LIBS}/whisper.cpp/CMakeLists.txt)
    add_subdirectory(${CHOPLAB_LIBS}/whisper.cpp whisper EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(whisper
        GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
        GIT_TAG v1.9.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(whisper)
endif()

set(CHOPLAB_CORE_SOURCES
    Source/Analysis/Analysis.cpp
    Source/Engine/Chopper.cpp
    Source/Engine/Renderer.cpp
    Source/Lyrics/Lyrics.cpp
)

set(CHOPLAB_COMMON_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

if(CHOPLAB_BUILD_TESTS)
    juce_add_console_app(ChopLabTests PRODUCT_NAME "ChopLabTests")
    target_sources(ChopLabTests PRIVATE Tests/AnalysisTests.cpp Tests/LyricsTests.cpp ${CHOPLAB_CORE_SOURCES})
    target_include_directories(ChopLabTests PRIVATE Source ${SIGNALSMITH_STRETCH_DIR}/.. ${SIGNALSMITH_LINEAR_PARENT})
    target_compile_definitions(ChopLabTests PRIVATE ${CHOPLAB_COMMON_DEFINITIONS} JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(ChopLabTests PRIVATE
        whisper
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

juce_add_plugin(ChopLab
    COMPANY_NAME "ChopLab"
    BUNDLE_ID com.choplab.choplab
    PLUGIN_MANUFACTURER_CODE Chlb
    PLUGIN_CODE Chp1
    FORMATS VST3 Standalone
    PRODUCT_NAME "ChopLab"
    DESCRIPTION "Sample chopper with BPM, key and beat detection"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Sampler)

target_sources(ChopLab PRIVATE
    ${CHOPLAB_CORE_SOURCES}
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/WaveformView.cpp
    Source/UI/SliceTable.cpp
    Source/UI/Panels.cpp)

target_include_directories(ChopLab PRIVATE Source ${SIGNALSMITH_STRETCH_DIR}/.. ${SIGNALSMITH_LINEAR_PARENT})
target_compile_definitions(ChopLab PUBLIC ${CHOPLAB_COMMON_DEFINITIONS} JUCE_VST3_CAN_REPLACE_VST2=0)

if(MSVC)
    target_compile_options(ChopLab PRIVATE /bigobj)
endif()

target_link_libraries(ChopLab
    PRIVATE
        whisper
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(CHOPLAB_BUILD_TESTS)
    juce_add_console_app(ChopLabHostTests PRODUCT_NAME "ChopLabHostTests")
    target_sources(ChopLabHostTests PRIVATE Tests/HostTests.cpp)
    target_compile_definitions(ChopLabHostTests PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(ChopLabHostTests PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
        juce::juce_recommended_config_flags)
endif()
