cmake_minimum_required(VERSION 3.21)
project(PluginShellExtension VERSION 0.94 LANGUAGES CXX RC)
# Windows version resources need a third number; "0.90" has none.
if(NOT PROJECT_VERSION_PATCH)
    set(PROJECT_VERSION_PATCH 0)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into explorer.exe and must not depend on a
# VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# bsa-reader (GPL-3.0, github.com/cecell/bsa-reader): BSA v103-105 with zlib
# and LZ4, used to read localized strings packed in archives. Pinned to the
# commit of tag v0.1.1; library changes go to that repo, then bump this pin.
# Fetched before the project-wide definitions below so it builds with its
# own flags (it inherits only the static CRT above).
include(FetchContent)
FetchContent_Declare(bsa_reader
    GIT_REPOSITORY https://github.com/cecell/bsa-reader.git
    GIT_TAG bbff0cbc5cd04d39bb3b50c470c579669c027dbf)  # v0.1.1
FetchContent_MakeAvailable(bsa_reader)
# beth-asset-metadata (GPL-3.0, github.com/cecell/beth-asset-metadata): header
# facts for the .tga and .fuz property handler. Pinned to the commit of tag
# v0.4.0; library changes go to that repo, then bump this pin.
FetchContent_Declare(asset_metadata
    GIT_REPOSITORY https://github.com/cecell/beth-asset-metadata.git
    GIT_TAG 9d0c51a3cb43866d9e0e2118a6f73a2c83074057)  # v0.4.0
FetchContent_MakeAvailable(asset_metadata)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    PSE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} PSE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PSE_VERSION_PATCH=${PROJECT_VERSION_PATCH} PSE_VERSION_STR="${PROJECT_VERSION}")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

# Parser + registry code shared by the DLL and the tools.
add_library(psecore STATIC
    src/plugin_parser.cpp
    src/registration.cpp)
target_include_directories(psecore PUBLIC src)
target_link_libraries(psecore PUBLIC shlwapi propsys)

add_library(PluginShellExtension SHARED
    src/dllmain.cpp
    src/context_menu.cpp
    src/hardlink_menu.cpp
    src/property_window.cpp
    src/property_handler.cpp
    src/asset_property_handler.cpp
    src/bsa_folder.cpp
    src/embedded_patterns.cpp
    src/disk_cache.cpp
    src/PluginShellExtension.def
    src/PluginShellExtension.rc
    src/PluginShellExtension.manifest)
# The ITM pattern catalog is compiled into the DLL as a resource.
set_source_files_properties(src/PluginShellExtension.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/itm_patterns.json)
target_link_libraries(PluginShellExtension PRIVATE psecore psepattern asset::metadata bsa::reader comctl32 uxtheme shell32 ole32 oleaut32 propsys
    windowscodecs shlwapi dwmapi)

# Subrecord pattern engine and the ITM check.
add_library(psepattern STATIC
    src/pattern_value.cpp
    src/pattern_engine.cpp
    src/inflate.cpp
    src/itm.cpp)
target_include_directories(psepattern PUBLIC src)
target_link_libraries(psepattern PRIVATE bsa::reader)

add_executable(itmcheck tools/itmcheck.cpp)
target_link_libraries(itmcheck PRIVATE psepattern)

add_executable(patterndump tools/patterndump.cpp)
target_link_libraries(patterndump PRIVATE psepattern)

add_executable(plugininfo tools/plugininfo.cpp)
target_link_libraries(plugininfo PRIVATE psecore)

add_executable(proptest tools/proptest.cpp)
target_link_libraries(proptest PRIVATE psecore shell32 ole32 shlwapi)

add_executable(assetproptest tools/assetproptest.cpp)
target_link_libraries(assetproptest PRIVATE psecore shell32 ole32 shlwapi propsys)

add_executable(bsabrowsetest tools/bsabrowsetest.cpp)
target_link_libraries(bsabrowsetest PRIVATE psecore shell32 ole32 shlwapi)

add_executable(menutest tools/menutest.cpp)
target_link_libraries(menutest PRIVATE psecore shell32 ole32)
add_executable(hardlinktest tools/hardlinktest.cpp)
target_link_libraries(hardlinktest PRIVATE shell32 ole32)

set_target_properties(PluginShellExtension PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The installer: build/PluginShellExtension-<version>-setup.exe, built with Inno
# Setup (6 or newer) when ISCC.exe is found (winget install JRSoftware.InnoSetup).
find_program(ISCC_EXE ISCC
    PATHS "$ENV{LOCALAPPDATA}/Programs/Inno Setup 7" "$ENV{ProgramFiles}/Inno Setup 7"
          "C:/Program Files (x86)/Inno Setup 7"
          "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6"
          "C:/Program Files (x86)/Inno Setup 6"
    NO_DEFAULT_PATH)
if(ISCC_EXE)
    set(PSE_SETUP ${CMAKE_BINARY_DIR}/PluginShellExtension-${PROJECT_VERSION}-setup.exe)
    add_custom_command(OUTPUT ${PSE_SETUP}
        COMMAND ${ISCC_EXE} /Q /DAppVersion=${PROJECT_VERSION} /DBinDir=${CMAKE_BINARY_DIR}/bin
            /DOutDir=${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/installer/PluginShellExtension.iss
        DEPENDS PluginShellExtension ${CMAKE_SOURCE_DIR}/installer/PluginShellExtension.iss
            ${CMAKE_SOURCE_DIR}/installer/PluginShellExtension.propdesc ${CMAKE_SOURCE_DIR}/LICENSE
            ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/docs/screenshot.png
            ${CMAKE_SOURCE_DIR}/installer/icon/PluginShellExtension.ico
        COMMENT "Building installer with Inno Setup")
    add_custom_target(installer ALL DEPENDS ${PSE_SETUP})
else()
    message(STATUS "Inno Setup (ISCC.exe) not found: skipping the installer")
endif()
