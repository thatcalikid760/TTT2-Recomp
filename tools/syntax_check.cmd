@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set INC=-I C:\ttt2\re\ppc -I C:\ttt2\tools\XenonRecomp\XenonUtils -I C:\ttt2\tools\XenonRecomp\thirdparty\simde
clang-cl /std:c++20 /EHsc /c /Fo"%TEMP%\sc.obj" %INC% %1
echo SYNTAX_EXIT=%errorlevel%
