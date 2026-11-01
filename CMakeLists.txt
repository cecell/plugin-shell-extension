cmake_minimum_required(VERSION 3.21)
project(PluginProperties VERSION 1.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into explorer.exe and must not depend on a
# VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    PP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} PP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PP_VERSION_PATCH=${PROJECT_VERSION_PATCH} PP_VERSION_STR="${PROJECT_VERSION}")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

# Parser + registry code shared by the DLL and the tools.
add_library(ppcore STATIC
    src/plugin_parser.cpp
    src/registration.cpp)
target_include_directories(ppcore PUBLIC src)
target_link_libraries(ppcore PUBLIC shlwapi)

add_library(PluginProperties SHARED
    src/dllmain.cpp
    src/context_menu.cpp
    src/property_window.cpp
    src/PluginProperties.def
    src/PluginProperties.rc
    src/PluginProperties.manifest)
target_link_libraries(PluginProperties PRIVATE ppcore comctl32 uxtheme shell32 ole32 oleaut32 propsys
    windowscodecs shlwapi dwmapi)

add_executable(plugininfo tools/plugininfo.cpp)
target_link_libraries(plugininfo PRIVATE ppcore)

add_executable(menutest tools/menutest.cpp)
target_link_libraries(menutest PRIVATE ppcore shell32 ole32)

set_target_properties(PluginProperties PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The installer: build/PluginProperties-<version>-setup.exe, built with Inno
# Setup (6 or newer) when ISCC.exe is found (winget install JRSoftware.InnoSetup).
find_program(ISCC_EXE ISCC
    PATHS "$ENV{LOCALAPPDATA}/Programs/Inno Setup 7" "$ENV{ProgramFiles}/Inno Setup 7"
          "C:/Program Files (x86)/Inno Setup 7"
          "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6"
          "C:/Program Files (x86)/Inno Setup 6"
    NO_DEFAULT_PATH)
if(ISCC_EXE)
    set(PP_SETUP ${CMAKE_BINARY_DIR}/PluginProperties-${PROJECT_VERSION}-setup.exe)
    add_custom_command(OUTPUT ${PP_SETUP}
        COMMAND ${ISCC_EXE} /Q /DAppVersion=${PROJECT_VERSION} /DBinDir=${CMAKE_BINARY_DIR}/bin
            /DOutDir=${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/installer/PluginProperties.iss
        DEPENDS PluginProperties ${CMAKE_SOURCE_DIR}/installer/PluginProperties.iss
        COMMENT "Building installer with Inno Setup")
    add_custom_target(installer ALL DEPENDS ${PP_SETUP})
else()
    message(STATUS "Inno Setup (ISCC.exe) not found: skipping the installer")
endif()
