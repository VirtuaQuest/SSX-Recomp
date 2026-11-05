@echo off
rem Builds the setup program with the Visual Studio C++ tools and opens it. It does the rest. Nothing is downloaded.
setlocal
cd /d "%~dp0"

set "INSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%INSTALLER%\vswhere.exe" goto no_tools
set "PATH=%INSTALLER%;%PATH%"

set "VS="
for /f "usebackq delims=" %%i in (`vswhere -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "VS=%%i"
if not defined VS for /f "usebackq delims=" %%i in (`vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "VS=%%i"
if not defined VS goto no_tools

if not exist out\setup mkdir out\setup
if exist out\setup\ARSetup.exe tasklist /fi "imagename eq ARSetup.exe" 2>nul | find /i "ARSetup.exe" >nul && goto open

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
echo Building the setup program...
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /Itools\ui /Fo:out\setup\ /Fe:out\setup\ARSetup.exe tools\setup\main.cpp tools\setup\package.cpp tools\ui\theme.cpp /link /subsystem:windows user32.lib gdi32.lib comctl32.lib comdlg32.lib ole32.lib shell32.lib bcrypt.lib >out\setup\setup_build.log
if errorlevel 1 (
    type out\setup\setup_build.log
    echo.
    echo The setup program did not build. The messages above say why.
    pause
    exit /b 1
)

:open
start "" out\setup\ARSetup.exe
exit /b 0

:no_tools
echo This needs the C++ tools of Visual Studio: "Desktop development with C++" in
echo Visual Studio Community or Build Tools, from Microsoft. Install those, then run this again.
pause
exit /b 1
