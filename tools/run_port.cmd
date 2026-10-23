@echo off
setlocal
rem Usage: run_port.cmd [build-directory] [log-file]
rem The optional log is resolved relative to the caller, before changing directory.
rem Native redirection avoids PowerShell's RedirectStandardError relay pipe.
for %%I in ("%~dp0..") do set "TTT2_PROJECT_ROOT=%%~fI"
set "TTT2_RUN_BUILD=build"
if not "%~1"=="" set "TTT2_RUN_BUILD=%~1"
set "TTT2_VFS_ROOT=%TTT2_PROJECT_ROOT%\re\game_files_x"
set "TTT2_RUN_EXE=%TTT2_PROJECT_ROOT%\port\%TTT2_RUN_BUILD%\tekken_recomp.exe"
set "TTT2_RUN_IMAGE=%TTT2_PROJECT_ROOT%\re\decrypted\Default_patched_tu3.bin"
set "TTT2_RUN_LOG="
if not "%~2"=="" for %%I in ("%~2") do set "TTT2_RUN_LOG=%%~fI"
if not exist "%TTT2_RUN_EXE%" (
    echo Native executable missing: "%TTT2_RUN_EXE%"
    echo Build it with tools\build_port.cmd first.
    exit /b 1
)
if not exist "%TTT2_RUN_IMAGE%" (
    echo Local game image missing: "%TTT2_RUN_IMAGE%"
    exit /b 1
)
if not exist "%TTT2_VFS_ROOT%\data000.bin" (
    echo Expanded local game archives missing: "%TTT2_VFS_ROOT%"
    exit /b 1
)
if not defined TTT2_RUN_LOG goto launch
if exist "%TTT2_RUN_LOG%\*" (
    echo Log path is a directory: "%TTT2_RUN_LOG%"
    exit /b 1
)
rem COPY reports open failures reliably; cmd redirection alone may preserve
rem the preceding command's successful ERRORLEVEL when the file cannot open.
copy /y nul "%TTT2_RUN_LOG%" >nul 2>&1
if errorlevel 1 (
    echo Cannot create log file: "%TTT2_RUN_LOG%"
    exit /b 1
)
:launch
pushd "%TTT2_PROJECT_ROOT%\port" || exit /b 1
if defined TTT2_RUN_LOG goto run_logged
"%TTT2_RUN_EXE%" "%TTT2_RUN_IMAGE%"
goto capture_exit
:run_logged
rem cmd opens the file before starting the game; a bad path prevents launch.
"%TTT2_RUN_EXE%" "%TTT2_RUN_IMAGE%" >"%TTT2_RUN_LOG%" 2>&1
:capture_exit
set "TTT2_RUN_EXIT=%ERRORLEVEL%"
popd
exit /b %TTT2_RUN_EXIT%
