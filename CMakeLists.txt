cmake_minimum_required(VERSION 3.21)
project(PluginProperties VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into explorer.exe and must not depend on a
# VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    PP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} PP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PP_VERSION_PATCH=${PROJECT_VERSION_PATCH} PP_VERSION_STR="${PROJECT_VERSION}")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

# Parser + registry code shared by the DLL, the setup program and the tools.
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

# Setup installs the PluginProperties.dll that sits next to it.
add_executable(PluginPropertiesSetup WIN32
    setup/setup.cpp
    setup/setup.rc
    setup/setup.manifest)
target_link_libraries(PluginPropertiesSetup PRIVATE ppcore shell32 advapi32 ole32 user32)
set_target_properties(PluginProperties PluginPropertiesSetup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The distributable: build/PluginProperties-<version>.zip with setup + DLL.
set(PP_ZIP ${CMAKE_BINARY_DIR}/PluginProperties-${PROJECT_VERSION}.zip)
add_custom_command(OUTPUT ${PP_ZIP}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/dist/README.txt ${CMAKE_BINARY_DIR}/bin/README.txt
    COMMAND ${CMAKE_COMMAND} -E tar cf ${PP_ZIP} --format=zip
        PluginPropertiesSetup.exe PluginProperties.dll README.txt
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    DEPENDS PluginProperties PluginPropertiesSetup ${CMAKE_SOURCE_DIR}/dist/README.txt)
add_custom_target(package_zip ALL DEPENDS ${PP_ZIP})
