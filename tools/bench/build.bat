@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /O2 /EHsc /std:c++17 mem_scaling.cpp /Fe:mem_scaling.exe
