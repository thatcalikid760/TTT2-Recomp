@echo off
rem Build XenosRecomp (Xenos shader recompiler) with clang-cl + Ninja, mirroring
rem build_xenonrecomp.cmd. Do NOT define UNLEASHED_RECOMP (gates Sonic-specific hacks).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cd /d "C:\ttt2\tools\XenosRecomp"
echo === Configuring ===
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )
echo === Building ===
"%CMAKE%" --build build
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo === Done ===
dir /b build\*.exe 2>nul
dir /b build\XenosRecomp\*.exe 2>nul
