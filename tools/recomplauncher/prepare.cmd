@echo off
rem Recomp Launcher's setup step (recomplauncher.json "prepare"), which works from a command line too: builds the game on
rem this PC from the player's own copy, without a window. It gets FFmpeg if it isn't here, builds the setup program
rem (ARPrepare.exe, Setup without its window) and runs it. The output is "progress <0 to 1> <what>" lines for the launcher
rem and plain lines for a person; the exit code is 0 when the game is built (the others are in docs\RECOMPLAUNCHER.md).
rem Usage: tools\recomplauncher\prepare.cmd --game <disc image or folder> [--dlc <folder or package>]
setlocal
cd /d "%~dp0..\.."

echo progress 0.000 Checking the build tools
set "INSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%INSTALLER%\vswhere.exe" goto no_tools
set "PATH=%INSTALLER%;%PATH%"
set "VS="
for /f "usebackq delims=" %%i in (`vswhere -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "VS=%%i"
if not defined VS for /f "usebackq delims=" %%i in (`vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "VS=%%i"
if not defined VS goto no_tools

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get_ffmpeg.ps1"
if errorlevel 1 exit /b 2

echo progress 0.005 Building the setup program
if not exist out\setup\prepare mkdir out\setup\prepare
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /Itools\ui /Fo:out\setup\prepare\ /Fe:out\setup\ARPrepare.exe tools\setup\main.cpp tools\setup\package.cpp tools\ui\theme.cpp /link /subsystem:console user32.lib gdi32.lib comctl32.lib comdlg32.lib ole32.lib shell32.lib bcrypt.lib >out\setup\prepare_build.log
if errorlevel 1 goto no_setup

out\setup\ARPrepare.exe --prepare %*
exit /b %errorlevel%

:no_setup
type out\setup\prepare_build.log
echo progress 0 Stopped: the setup program didn't build. The messages are in out\setup\prepare_build.log.
exit /b 5

:no_tools
echo progress 0 Stopped: this PC needs Visual Studio 2022 Build Tools, with Desktop development with C++, the Windows SDK and C++ Clang tools for Windows.
echo Install Visual Studio Build Tools 2022 from https://visualstudio.microsoft.com/downloads/ with "Desktop development with C++",
echo and in it "C++ Clang tools for Windows", then set the game up again.
exit /b 2
