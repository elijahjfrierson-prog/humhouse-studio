cmake_minimum_required(VERSION 3.22)

project(HumHouseStudio VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    add_compile_options(/bigobj)
endif()

include(FetchContent)
set(JUCE_VERSION 8.0.4 CACHE STRING "JUCE version to fetch")
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${JUCE_VERSION}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

set(HHS_CORE_SOURCES
    Source/Model.cpp
    Source/NoteTools.cpp
    Source/Instruments.cpp
    Source/Effects.cpp
    Source/StudioEffects.cpp
    Source/Engine.cpp
    Source/ProjectIO.cpp
    Source/DrumSynth.cpp
)

set(HHS_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_APPLICATION_NAME_STRING="HumHouse Studio"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
)
if(APPLE)
    list(APPEND HHS_DEFS JUCE_PLUGINHOST_AU=1)
endif()

set(HHS_LIBS
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
)

option(HHS_TESTS_ONLY "Build only the headless test target" OFF)
if(NOT HHS_TESTS_ONLY)
juce_add_gui_app(HumHouseStudio
    PRODUCT_NAME "HumHouse Studio"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/humhouse-icon.png"
    COMPANY_NAME "HumHouse"
    BUNDLE_ID "com.humhouse.studio"
    VERSION ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "HumHouse Studio records vocals and instruments from your audio input."
    HARDENED_RUNTIME_ENABLED FALSE
    DOCUMENT_EXTENSIONS hhs
)

target_sources(HumHouseStudio PRIVATE
    ${HHS_CORE_SOURCES}
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/ChannelRack.cpp
    Source/PianoRoll.cpp
    Source/Playlist.cpp
    Source/MixerView.cpp
    Source/Browser.cpp
    Source/ToolDialogs.cpp
    Source/EffectEditors.cpp
    Source/Theme.cpp
    Source/ScopeView.cpp
)
target_compile_definitions(HumHouseStudio PRIVATE ${HHS_DEFS})
target_link_libraries(HumHouseStudio PRIVATE ${HHS_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

# --- Tests: engine, effects, note tools and project I/O, headless -------------
enable_testing()
juce_add_console_app(HHSTests PRODUCT_NAME "HHSTests")
target_sources(HHSTests PRIVATE ${HHS_CORE_SOURCES} tests/CoreTests.cpp)
target_compile_definitions(HHSTests PRIVATE ${HHS_DEFS})
target_link_libraries(HHSTests PRIVATE ${HHS_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME HHSTests COMMAND HHSTests)

# classic tattoo lettering (SIL Open Font License, see Resources/Fonts/OFL-*.txt)
if(TARGET HumHouseStudio)
    juce_add_binary_data(HHSFonts SOURCES Resources/Fonts/PirataOne-Regular.ttf Resources/Fonts/Rye-Regular.ttf)
    target_link_libraries(HumHouseStudio PRIVATE HHSFonts)
endif()
