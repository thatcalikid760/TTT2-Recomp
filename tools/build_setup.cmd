@echo off
rem Build the installer (setup\ttt2_setup.exe + the asInvoker test twin ttt2_setup_probe.exe) in an
rem already-configured build directory. It embeds that directory's tekken_recomp.exe, relinking
rem the game first if its sources changed. Run tools\build_port.cmd once before this.
rem Usage: build_setup.cmd [build-directory] [parallel-jobs]
rem Example: build_setup.cmd build-o1 8
setlocal
set "PORT_BUILD_DIR=%~1"
if not defined PORT_BUILD_DIR set "PORT_BUILD_DIR=build-o1"
set "PORT_BUILD_JOBS=%~2"
if not defined PORT_BUILD_JOBS set "PORT_BUILD_JOBS=8"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "C:\ttt2\port"
if not exist "%PORT_BUILD_DIR%\CMakeCache.txt" (
    echo Build directory "%PORT_BUILD_DIR%" is not configured. Run tools\build_port.cmd first.
    exit /b 1
)
"%CMAKE%" --build "%PORT_BUILD_DIR%" --target ttt2_setup ttt2_setup_probe --parallel "%PORT_BUILD_JOBS%"
set "PORT_BUILD_RESULT=%errorlevel%"
echo SETUP_BUILD_EXIT=%PORT_BUILD_RESULT%
exit /b %PORT_BUILD_RESULT%
