@echo off
rem Builds Release x64 with the MSVC 14.44 (v143) toolset.
rem Output: build\bin\PluginShellExtension.dll and, when Inno Setup 6 is installed,
rem build\PluginShellExtension-<version>-setup.exe.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
rem Configure every time so a newly installed Inno Setup is picked up.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >nul
if errorlevel 1 exit /b 1
cmake --build build
