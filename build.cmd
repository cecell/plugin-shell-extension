@echo off
rem Builds Release x64 with the MSVC 14.44 (v143) toolset.
rem Output: build\PluginPropertiesSetup.exe (installer with the DLL embedded).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build
