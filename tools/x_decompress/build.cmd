@echo off
setlocal
set MS=C:\ttt2\tools\XenonRecomp\thirdparty\libmspack\libmspack\mspack
"C:\Program Files\LLVM\bin\clang-cl.exe" /nologo /O2 /std:c++20 /EHsc /I "%MS%" ^
  "C:\ttt2\tools\x_decompress\x_decompress.cpp" "%MS%\lzxd.c" ^
  /Fo"C:\ttt2\tools\x_decompress\\" /Fe"C:\ttt2\tools\x_decompress\x_decompress.exe"
echo XDEC_BUILD_EXIT=%ERRORLEVEL%
