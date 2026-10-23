@echo off
setlocal
rem Kept for compatibility: the title-storage emulation that unlocks the 10 gated fighters
rem (Ancient Ogre, Angel, Kunimitsu, Michelle, Miharu, Sebastian, Slim Bob, Violet,
rem Dr. Bosconovitch, Unknown) is now ON by default in every session, so this is the same as
rem run_port.cmd. Set TTT2_TITLE_SERVER=0 before run_port.cmd to turn it off. Paid DLC
rem (costumes, music) is untouched.
rem Usage: run_port_unlock.cmd [build-directory] [log-file]   (same as run_port.cmd)
set "TTT2_TITLE_SERVER=1"
call "%~dp0run_port.cmd" %*
exit /b %ERRORLEVEL%
