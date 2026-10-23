@echo off
rem ttt2admin <command...>: an admin command in the running ttt2-server (README.md "Admin commands"), run from this
rem folder with the server.toml next to it. Use the Windows account the server runs as.
rem e.g. ttt2admin players / ttt2admin players all jin / ttt2admin ban <steam id> 7 cheating / ttt2admin panel-setup
"%~dp0ttt2-server.exe" -config "%~dp0server.toml" admin %*
exit /b %errorlevel%
