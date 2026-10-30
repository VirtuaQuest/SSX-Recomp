@echo off
rem Usage: tools\build.bat [target]   (ARRecomp, ARLauncher, ARShaderTool; everything when omitted)
rem Configures out\build on first use: Ninja, clang-cl, RelWithDebInfo. LLVM_DIR overrides the portable LLVM location.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined LLVM_DIR set "LLVM_DIR=C:\Downloads\Repos\_tools\llvm"
set "PATH=%LLVM_DIR%\bin;%PATH%"
cd /d "%~dp0.."
if not exist out\build\build.ninja cmake -S . -B out\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
if "%1"=="" (cmake --build out\build -- -k 0) else (cmake --build out\build --target %1 -- -k 0)
