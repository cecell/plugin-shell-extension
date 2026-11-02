cmake_minimum_required(VERSION 3.21)
project(PluginShellExtension VERSION 0.90 LANGUAGES CXX RC)
# Windows version resources need a third number; "0.90" has none.
if(NOT PROJECT_VERSION_PATCH)
    set(PROJECT_VERSION_PATCH 0)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into explorer.exe and must not depend on a
# VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    src/property_window.cpp
    src/property_handler.cpp
    src/PluginShellExtension.def
    src/PluginShellExtension.rc
    src/PluginShellExtension.manifest)
target_link_libraries(PluginShellExtension PRIVATE psecore comctl32 uxtheme shell32 ole32 oleaut32 propsys
    windowscodecs shlwapi dwmapi)

add_executable(plugininfo tools/plugininfo.cpp)
target_link_libraries(plugininfo PRIVATE psecore)

add_executable(proptest tools/proptest.cpp)
target_link_libraries(proptest PRIVATE psecore shell32 ole32 shlwapi)

add_executable(menutest tools/menutest.cpp)
target_link_libraries(menutest PRIVATE psecore shell32 ole32)

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
