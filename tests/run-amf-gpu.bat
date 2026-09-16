@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHsc /W4 amf_gpu_tests.cpp ..\pc-host\amf_encoder.cpp /Fo:out\ /Fe:out\amf_gpu_tests.exe /link d3d11.lib dxgi.lib ole32.lib
if errorlevel 1 exit /b 1
out\amf_gpu_tests.exe %*
