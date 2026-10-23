@echo off
rem Configure FFmpeg n7.1 (tools\ffmpeg_dl\FFmpeg-n7.1) into tools\ffmpeg_dl\build for the XMA-only
rem static lib, with the options listed at the top of port\third_party\ffmpeg_xma\CMakeLists.txt.
rem Only configure runs here; the CMake build compiles the needed sources itself.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
if not exist "C:\ttt2\tools\ffmpeg_dl\build" mkdir "C:\ttt2\tools\ffmpeg_dl\build"
cd /d "C:\ttt2\tools\ffmpeg_dl\build"
"C:\Program Files\Git\usr\bin\bash.exe" "C:\ttt2\tools\configure_ffmpeg.sh"
set "RESULT=%errorlevel%"
echo FFMPEG_CONFIGURE_EXIT=%RESULT%
exit /b %RESULT%
