cmake_minimum_required(VERSION 3.22)

project(HumHouseStudio VERSION 1.5.4 LANGUAGES C CXX)

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
    Source/PluginSandbox.cpp
    Source/ProjectIO.cpp
    Source/ProjectImport.cpp
    Source/DrumSynth.cpp
    Source/AudioEncode.cpp
    Source/StemSplit.cpp
    Source/Purify.cpp
    Source/HumLab.cpp
    Source/License.cpp
    Source/Drummer/DrummerCore.cpp
    Source/Drummer/GrooveCorpus.cpp
    Source/Drummer/PerformanceEngine.cpp
    Source/Drummer/KitEngine.cpp
)

# Linux needs curl for HTTPS (license activation); macOS / Windows / iOS use the system's own
if(UNIX AND NOT APPLE)
    set(HHS_USE_CURL 1)
else()
    set(HHS_USE_CURL 0)
endif()
set(HHS_LICENSE_URL "https://humhouse-license.humhouse.workers.dev" CACHE STRING "HumHouse license server (regular build activation)")
set(HHS_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=${HHS_USE_CURL}
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    HHS_LICENSE_URL="${HHS_LICENSE_URL}"
)
# iPhone / iPad: AUv3 plug-ins only (no VST3 on iOS)
if(IOS)
    list(APPEND HHS_DEFS JUCE_PLUGINHOST_VST3=0)
else()
    list(APPEND HHS_DEFS JUCE_PLUGINHOST_VST3=1)
endif()
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
# Session Drummer groove corpus (Groove MIDI Dataset, CC-BY 4.0), from HumHouse Drums X
juce_add_binary_data(HHSDrummerData HEADER_NAME DrummerData.h NAMESPACE DrummerData SOURCES Resources/Drummer/rock_corpus.hhc)
list(APPEND HHS_LIBS HHSDrummerData)
# fingerprints of every sold key (scripts/hhs_keyprints.py): a real key unlocks even with the license server down
juce_add_binary_data(HHSKeyprints HEADER_NAME KeyprintData.h NAMESPACE KeyprintData SOURCES Resources/License/keyprints.bin)
list(APPEND HHS_LIBS HHSKeyprints)
if(APPLE OR WIN32)
    list(APPEND HHS_LIBS juce::juce_video)   # VIDEO tab player (AVFoundation / Media Foundation)
endif()
if(APPLE)
    list(APPEND HHS_LIBS "-framework AudioToolbox" "-framework CoreFoundation")
endif()

option(HHS_TESTS_ONLY "Build only the headless test target" OFF)

# VIDEO tab editor (InSequence FX, needs FFmpeg) - desktop builds only; iPhone keeps the simple video player
set(HHS_ISFX_SOURCES "")
if(NOT IOS AND NOT HHS_TESTS_ONLY)
    set(FFMPEG_DIR "" CACHE PATH "FFmpeg install prefix (Windows: BtbN lgpl-shared build)")
    add_library(hhs_ffmpeg INTERFACE)
    set(HHS_WITH_ISFX OFF)
    if(WIN32)
        if(FFMPEG_DIR)
            target_include_directories(hhs_ffmpeg INTERFACE "${FFMPEG_DIR}/include")
            foreach(l avformat avcodec avutil swscale swresample)
                target_link_libraries(hhs_ffmpeg INTERFACE "${FFMPEG_DIR}/lib/${l}.lib")
            endforeach()
            set(HHS_WITH_ISFX ON)
        endif()
    else()
        if(FFMPEG_DIR)
            set(ENV{PKG_CONFIG_PATH} "${FFMPEG_DIR}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        endif()
        find_package(PkgConfig)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
            if(FFMPEG_FOUND)
                target_link_libraries(hhs_ffmpeg INTERFACE PkgConfig::FFMPEG)
                set(HHS_WITH_ISFX ON)
            endif()
        endif()
    endif()
    if(HHS_WITH_ISFX)
        message(STATUS "VIDEO tab: InSequence FX editor ON")
        juce_add_binary_data(ISFXData HEADER_NAME ISFXData.h NAMESPACE ISFXData SOURCES Resources/ISFX/Michroma-Regular.ttf Resources/ISFX/insequence-mark.png)
        list(APPEND HHS_LIBS ISFXData hhs_ffmpeg juce::juce_opengl)
        list(APPEND HHS_DEFS HHS_WITH_ISFX=1 __STDC_CONSTANT_MACROS)
        file(GLOB HHS_ISFX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/ISFX/*.cpp)
    else()
        message(WARNING "VIDEO tab: FFmpeg not found, InSequence FX editor OFF")
    endif()
endif()
if(NOT HHS_TESTS_ONLY)
# classic tattoo lettering (SIL Open Font License, see Resources/Fonts/OFL-*.txt)
juce_add_binary_data(HHSFonts SOURCES Resources/Fonts/PirataOne-Regular.ttf Resources/Fonts/Rye-Regular.ttf Resources/Fonts/Barlow-Regular.ttf Resources/Fonts/Barlow-SemiBold.ttf)

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
        DOCUMENT_EXTENSIONS hhs hhpack hhpreset hhchain
        # iPhone / iPad (ignored on desktop)
        IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        REQUIRES_FULL_SCREEN TRUE
        STATUS_BAR_HIDDEN TRUE
        BACKGROUND_AUDIO_ENABLED TRUE
        FILE_SHARING_ENABLED TRUE
        DOCUMENT_BROWSER_ENABLED TRUE
        TARGETED_DEVICE_FAMILY "1,2"
        NEEDS_CURL TRUE
    )
    if(IOS)   # bundled kits / sounds go inside the .app (bundledSoundsRoot = next to the executable)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Sounds" "$<TARGET_BUNDLE_DIR:${target}>/Sounds")
    endif()
    target_sources(${target} PRIVATE
        ${HHS_ISFX_SOURCES}
        ${HHS_CORE_SOURCES}
        Source/Main.cpp
        Source/MainComponent.cpp
        Source/ChannelRack.cpp
        Source/PianoRoll.cpp
        Source/Playlist.cpp
        Source/MixerView.cpp
        Source/LanesView.cpp
        Source/DJView.cpp
    Source/PatchBay.cpp
    Source/VideoView.cpp
    Source/Lang.cpp
        Source/Browser.cpp
        Source/ToolDialogs.cpp
        Source/EffectEditors.cpp
        Source/Theme.cpp
        Source/ScopeView.cpp
    )
    target_compile_definitions(${target} PRIVATE ${HHS_DEFS} JUCE_APPLICATION_NAME_STRING="${product}" HHS_LICENSED=${licensed} HHS_KEY_GATE=${licensed})
    if(APPLE AND NOT IOS AND HHS_WITH_ISFX)   # FFmpeg dylibs ship in Contents/Frameworks
        target_link_options(${target} PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
    endif()
    target_link_libraries(${target} PRIVATE ${HHS_LIBS} HHSFonts
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()
hhs_add_app(HumHouseStudio "HumHouse Studio" "com.humhouse.studio" 1)
hhs_add_app(HumHouseStudioMaster "HumHouse Studio Master" "com.humhouse.studio.master" 0)
endif()

# --- Tests: engine, effects, note tools and project I/O, headless -------------
if(NOT IOS)
enable_testing()
juce_add_console_app(HHSTests PRODUCT_NAME "HHSTests" NEEDS_CURL TRUE)
target_sources(HHSTests PRIVATE ${HHS_CORE_SOURCES} tests/CoreTests.cpp)
target_compile_definitions(HHSTests PRIVATE ${HHS_DEFS} JUCE_APPLICATION_NAME_STRING="HumHouse Studio")
target_link_libraries(HHSTests PRIVATE ${HHS_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME HHSTests COMMAND HHSTests)
endif()

if(TARGET HHSTests AND MSVC)
    # symbols for readable crash backtraces in CI
    target_compile_options(HHSTests PRIVATE /Zi)
    # the test main holds many Engines on its stack; Windows gives only 1 MB by default
    target_link_options(HHSTests PRIVATE /DEBUG /STACK:67108864)
endif()
