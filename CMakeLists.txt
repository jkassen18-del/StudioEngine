cmake_minimum_required(VERSION 3.22)
project(StudioEngine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

option(STUDIO_ENGINE_BUILD_PLUGIN "Build the VST3/AU/Standalone plugin (downloads JUCE)" ON)
option(STUDIO_ENGINE_BUILD_TOOLS  "Build the command-line renderer and tests" ON)

# ------------------------------------------------------------------------------
# Framework-free DSP core
# ------------------------------------------------------------------------------
add_library(studio_engine_dsp INTERFACE)
target_include_directories(studio_engine_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp)

if(STUDIO_ENGINE_BUILD_TOOLS)
    add_executable(studio_engine_cli tools/cli/main.cpp)
    target_link_libraries(studio_engine_cli PRIVATE studio_engine_dsp)
    set_target_properties(studio_engine_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    enable_testing()
    add_executable(studio_engine_tests tests/test_dsp.cpp)
    target_link_libraries(studio_engine_tests PRIVATE studio_engine_dsp)
    add_test(NAME dsp_tests COMMAND studio_engine_tests)
endif()

# ------------------------------------------------------------------------------
# Plugin (JUCE)
# ------------------------------------------------------------------------------
if(STUDIO_ENGINE_BUILD_PLUGIN)
    set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional; otherwise downloaded)")
    if(JUCE_DIR)
        add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
    else()
        include(FetchContent)
        FetchContent_Declare(juce
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.4
            GIT_SHALLOW ON)
        FetchContent_MakeAvailable(juce)
    endif()

    set(STUDIO_ENGINE_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND STUDIO_ENGINE_FORMATS AU)
    endif()

    juce_add_plugin(StudioEngine
        COMPANY_NAME "Studio Engine"
        PLUGIN_MANUFACTURER_CODE StEn
        PLUGIN_CODE StEg
        PRODUCT_NAME "Studio Engine"
        FORMATS ${STUDIO_ENGINE_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES Fx Restoration Mastering
        AU_MAIN_TYPE kAudioUnitType_Effect
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(StudioEngine PRIVATE
        Source/plugin/PluginProcessor.cpp
        Source/plugin/PluginEditor.cpp)

    target_compile_definitions(StudioEngine PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(StudioEngine
        PRIVATE
            studio_engine_dsp
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
