cmake_minimum_required(VERSION 3.22)
project(StudioEngine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

option(STUDIO_ENGINE_BUILD_PLUGIN "Build the VST3/AU/Standalone plugin (downloads JUCE)" ON)
option(STUDIO_ENGINE_BUILD_TOOLS  "Build the command-line renderer and tests" ON)
option(STUDIO_ENGINE_BUILD_UPDATER_GUI "Build the App Updater desktop app (needs JUCE)" ON)

# ------------------------------------------------------------------------------
# Framework-free DSP core
# ------------------------------------------------------------------------------
add_library(studio_engine_dsp INTERFACE)
target_include_directories(studio_engine_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp)

# ------------------------------------------------------------------------------
# App Updater core: replaces an installed program's exe with a new one
# (backup, verification, rollback). Framework-free; used by the CLI, tests and GUI.
# ------------------------------------------------------------------------------
add_library(app_updater_core STATIC
    tools/app_updater/UpdaterCore.cpp
    tools/app_updater/Platform.cpp)
target_include_directories(app_updater_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/app_updater)
if(WIN32)
    target_link_libraries(app_updater_core PUBLIC version shell32 user32 advapi32)
    target_compile_definitions(app_updater_core PUBLIC UNICODE _UNICODE)
endif()
# The updater ships as a single portable exe, so link the C++ runtime statically on Windows.
set(APP_UPDATER_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET app_updater_core PROPERTY MSVC_RUNTIME_LIBRARY ${APP_UPDATER_MSVC_RUNTIME})

if(STUDIO_ENGINE_BUILD_TOOLS)
    add_executable(studio_engine_cli tools/cli/main.cpp)
    target_link_libraries(studio_engine_cli PRIVATE studio_engine_dsp)
    set_target_properties(studio_engine_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    enable_testing()
    add_executable(studio_engine_tests tests/test_dsp.cpp)
    target_link_libraries(studio_engine_tests PRIVATE studio_engine_dsp)
    add_test(NAME dsp_tests COMMAND studio_engine_tests)

    # App Updater command line + tests
    add_executable(app_updater_cli tools/app_updater/cli_main.cpp)
    target_link_libraries(app_updater_cli PRIVATE app_updater_core)
    set_property(TARGET app_updater_cli PROPERTY MSVC_RUNTIME_LIBRARY ${APP_UPDATER_MSVC_RUNTIME})
    set_target_properties(app_updater_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    add_executable(app_updater_tests tests/test_updater.cpp)
    target_link_libraries(app_updater_tests PRIVATE app_updater_core)
    set_property(TARGET app_updater_tests PROPERTY MSVC_RUNTIME_LIBRARY ${APP_UPDATER_MSVC_RUNTIME})
    add_test(NAME updater_tests COMMAND app_updater_tests)
endif()

# ------------------------------------------------------------------------------
# Plugin (JUCE)
# ------------------------------------------------------------------------------
if(STUDIO_ENGINE_BUILD_PLUGIN OR STUDIO_ENGINE_BUILD_UPDATER_GUI)
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
endif()

if(STUDIO_ENGINE_BUILD_PLUGIN)

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

# ------------------------------------------------------------------------------
# App Updater desktop app (JUCE)
# ------------------------------------------------------------------------------
if(STUDIO_ENGINE_BUILD_UPDATER_GUI)
    juce_add_gui_app(AppUpdater
        PRODUCT_NAME "App Updater"
        COMPANY_NAME "Studio Engine"
        VERSION 1.0.0
        BUNDLE_ID com.studioengine.appupdater)

    target_sources(AppUpdater PRIVATE tools/app_updater/gui/Main.cpp)
    set_property(TARGET AppUpdater PROPERTY MSVC_RUNTIME_LIBRARY ${APP_UPDATER_MSVC_RUNTIME})

    target_compile_definitions(AppUpdater PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AppUpdater,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AppUpdater,JUCE_VERSION>")

    target_link_libraries(AppUpdater
        PRIVATE
            app_updater_core
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
