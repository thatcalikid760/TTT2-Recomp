@echo off
rem Compile the recompiled PPC C++ (re/ppc) into a static lib to prove Gate-1 "compiles".
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cd /d "C:\ttt2\port"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )
"%CMAKE%" --build build --target tekken_ppc
echo PPC_LIB_BUILD_EXIT=%errorlevel%
