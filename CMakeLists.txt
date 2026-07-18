cmake_minimum_required(VERSION 3.15)
project(DrumCrakerVST VERSION 1.3.4)

# Define plugin version
set(PLUGIN_VERSION_MAJOR 1)
set(PLUGIN_VERSION_MINOR 3)
set(PLUGIN_VERSION_PATCH 4)
set(PLUGIN_VERSION "${PLUGIN_VERSION_MAJOR}.${PLUGIN_VERSION_MINOR}.${PLUGIN_VERSION_PATCH}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DRUMCRAKER_ENABLE_NATIVE_OPTIMIZATIONS "Enable CPU-specific optimizations (-march=native, AVX2, etc.)" OFF)
set(DRUMCRAKER_MACOS_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version for compatibility builds")

# JUCE
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(STATUS "JUCE not found. Downloading...")
    include(FetchContent)
    FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
    )
    FetchContent_MakeAvailable(juce)
else()
    add_subdirectory(JUCE)
endif()

juce_add_plugin(DrumCrakerVST
    COMPANY_NAME "Wamphyre"
    PLUGIN_MANUFACTURER_CODE Wamp
    PLUGIN_CODE Dcvt
    FORMATS VST3 LV2
    LV2URI https://github.com/Wamphyre/DrumCraker
    PRODUCT_NAME "DrumCraker"
    VST3_CATEGORIES Instrument Sampler
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_SYNTH TRUE
    IS_MIDI_EFFECT FALSE
)

target_sources(DrumCrakerVST PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/DrumKitLoader.cpp
    src/SampleEngine.cpp
    src/VoiceManager.cpp
)

target_compile_definitions(DrumCrakerVST PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
    PLUGIN_VERSION="${PLUGIN_VERSION}"
)

target_link_libraries(DrumCrakerVST 
    PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Platform-specific compiler and linker options
if(MSVC)
    target_compile_options(DrumCrakerVST PRIVATE /O2 /DNDEBUG)
    if(DRUMCRAKER_ENABLE_NATIVE_OPTIMIZATIONS)
        target_compile_options(DrumCrakerVST PRIVATE /arch:AVX2)
    endif()

    # Automatic cleanup and release organization for Windows
    add_custom_command(TARGET DrumCrakerVST_VST3 POST_BUILD
        COMMAND powershell -Command "$bundle = (Get-Item '$<TARGET_FILE:DrumCrakerVST_VST3>').Directory.Parent.Parent.FullName; New-Item -ItemType Directory -Path '${CMAKE_CURRENT_SOURCE_DIR}/releases' -Force; Copy-Item -Path $bundle -Destination '${CMAKE_CURRENT_SOURCE_DIR}/releases/DrumCraker.vst3' -Recurse -Force; New-Item -ItemType Directory -Path '${CMAKE_CURRENT_SOURCE_DIR}/releases/DrumCraker.vst3/Contents/Resources' -Force; Copy-Item -Path '${CMAKE_CURRENT_SOURCE_DIR}/assets/background.png' -Destination '${CMAKE_CURRENT_SOURCE_DIR}/releases/DrumCraker.vst3/Contents/Resources/background.png' -Force; Write-Host 'Background image copied successfully'"
        VERBATIM
        COMMENT "Organizing Windows release in /releases folder..."
    )
else()
    target_compile_options(DrumCrakerVST PRIVATE -O3 -funroll-loops -fomit-frame-pointer -DNDEBUG)
    if(DRUMCRAKER_ENABLE_NATIVE_OPTIMIZATIONS)
        target_compile_options(DrumCrakerVST PRIVATE -march=native -mtune=native)
    endif()
    if(APPLE)
        if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
            set(CMAKE_OSX_DEPLOYMENT_TARGET "${DRUMCRAKER_MACOS_DEPLOYMENT_TARGET}" CACHE STRING "macOS deployment target" FORCE)
        endif()
        message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    elseif(UNIX AND NOT APPLE)
        # Linux and FreeBSD
        target_link_options(DrumCrakerVST PRIVATE -s)
        
        if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
            # FreeBSD-specific configurations
            target_link_libraries(DrumCrakerVST PRIVATE execinfo)
        endif()
    endif()
endif()

# Keep direct CMake builds self-contained. Both Linux and macOS VST3 binaries
# live one directory below Contents, alongside the Resources directory.
if(TARGET DrumCrakerVST_VST3 AND NOT MSVC)
    add_custom_command(TARGET DrumCrakerVST_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:DrumCrakerVST_VST3>/../Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/background.png"
            "$<TARGET_FILE_DIR:DrumCrakerVST_VST3>/../Resources/background.png"
        COMMENT "Copying VST3 resources"
    )
endif()
