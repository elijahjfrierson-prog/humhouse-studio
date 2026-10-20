cmake_minimum_required(VERSION 3.22)

project(HumHouseStudio VERSION 1.2.5 LANGUAGES C CXX)

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
    Source/AudioEncode.cpp
    Source/License.cpp
)

set(HHS_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
)
if(APPLE)
    list(APPEND HHS_DEFS JUCE_PLUGINHOST_AU=1)
endif()

# MP3 export: shine fixed-point MP3 encoder (LGPL-2.0), built as its own static library
FetchContent_Declare(shine GIT_REPOSITORY https://github.com/toots/shine.git GIT_TAG 3.1.1)
FetchContent_MakeAvailable(shine)
set(SHINE_DIR ${shine_SOURCE_DIR}/src/lib)
add_library(hhs_shine STATIC
    ${SHINE_DIR}/bitstream.c ${SHINE_DIR}/huffman.c ${SHINE_DIR}/l3bitstream.c ${SHINE_DIR}/l3loop.c
    ${SHINE_DIR}/l3mdct.c ${SHINE_DIR}/l3subband.c ${SHINE_DIR}/layer3.c ${SHINE_DIR}/reservoir.c ${SHINE_DIR}/tables.c)
target_include_directories(hhs_shine PUBLIC ${SHINE_DIR})
if(MSVC)
    target_compile_definitions(hhs_shine PRIVATE inline=__inline _CRT_SECURE_NO_WARNINGS)
    target_compile_options(hhs_shine PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/shine_msvc.h")
else()
    target_compile_options(hhs_shine PRIVATE -w)
endif()

set(HHS_LIBS
    hhs_shine
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_cryptography
)
if(APPLE)
    list(APPEND HHS_LIBS "-framework AudioToolbox" "-framework CoreFoundation")
endif()

option(HHS_TESTS_ONLY "Build only the headless test target" OFF)
if(NOT HHS_TESTS_ONLY)
# classic tattoo lettering (SIL Open Font License, see Resources/Fonts/OFL-*.txt)
juce_add_binary_data(HHSFonts SOURCES Resources/Fonts/PirataOne-Regular.ttf Resources/Fonts/Rye-Regular.ttf)

# "HumHouse Studio" = the stable app (HHS_LICENSED=1: Welcome screen, no new features);
# "HumHouse Studio Master" = gets every new feature first
function(hhs_add_app target product bundle licensed)
    juce_add_gui_app(${target}
        PRODUCT_NAME "${product}"
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/humhouse-icon.png"
        COMPANY_NAME "HumHouse"
        BUNDLE_ID "${bundle}"
        VERSION ${PROJECT_VERSION}
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "${product} records vocals and instruments from your audio input."
        HARDENED_RUNTIME_ENABLED FALSE
        DOCUMENT_EXTENSIONS hhs
    )
    target_sources(${target} PRIVATE
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
    target_compile_definitions(${target} PRIVATE ${HHS_DEFS} JUCE_APPLICATION_NAME_STRING="${product}" HHS_LICENSED=${licensed})
    target_link_libraries(${target} PRIVATE ${HHS_LIBS} HHSFonts
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()
hhs_add_app(HumHouseStudio "HumHouse Studio" "com.humhouse.studio" 1)
hhs_add_app(HumHouseStudioMaster "HumHouse Studio Master" "com.humhouse.studio.master" 0)
endif()

# --- Tests: engine, effects, note tools and project I/O, headless -------------
enable_testing()
juce_add_console_app(HHSTests PRODUCT_NAME "HHSTests")
target_sources(HHSTests PRIVATE ${HHS_CORE_SOURCES} tests/CoreTests.cpp)
target_compile_definitions(HHSTests PRIVATE ${HHS_DEFS} JUCE_APPLICATION_NAME_STRING="HumHouse Studio")
target_link_libraries(HHSTests PRIVATE ${HHS_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME HHSTests COMMAND HHSTests)
