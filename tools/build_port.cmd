@echo off
rem Configure + build the full port (tekken_recomp.exe = recompiled game lib + runtime).
rem Usage: build_port.cmd [build-directory] [PPC-optimization-level: 0/1/2] [parallel-jobs] [local-saves: ON/OFF]
rem Example: build_port.cmd build-o1 1 8
setlocal
set "PORT_BUILD_DIR=%~1"
if not defined PORT_BUILD_DIR set "PORT_BUILD_DIR=build"
set "PORT_PPC_OPT=%~2"
if not defined PORT_PPC_OPT set "PORT_PPC_OPT=0"
set "PORT_BUILD_JOBS=%~3"
if not defined PORT_BUILD_JOBS set "PORT_BUILD_JOBS=8"
rem Configure options under the same compiler environment as the build. Omitting
rem the fourth argument preserves the existing build directory's save setting.
rem Optimization is selected per target in CMakeLists. Keep the common build type
rem empty so CMake's Windows Debug default cannot inject /MDd, /Od or checked STL
rem iterators into performance builds after a fresh configure or cache reset.
set "PORT_SAVE_CMAKE="
if "%~4"=="" goto save_option_ready
if /i "%~4"=="ON" set "PORT_SAVE_CMAKE=-DTTT2_LOCAL_SAVES=ON"
if /i "%~4"=="OFF" set "PORT_SAVE_CMAKE=-DTTT2_LOCAL_SAVES=OFF"
if not defined PORT_SAVE_CMAKE (
    echo Local saves must be ON or OFF.
    exit /b 2
)
:save_option_ready
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cd /d "C:\ttt2\port"
"%CMAKE%" -S . -B "%PORT_BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE= -DTTT2_PPC_OPT_LEVEL=%PORT_PPC_OPT% %PORT_SAVE_CMAKE%
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )
"%CMAKE%" --build "%PORT_BUILD_DIR%" --target tekken_recomp --parallel "%PORT_BUILD_JOBS%"
set "PORT_BUILD_RESULT=%errorlevel%"
echo PORT_BUILD_EXIT=%PORT_BUILD_RESULT%
exit /b %PORT_BUILD_RESULT%
