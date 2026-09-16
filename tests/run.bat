@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHsc /W4 stream_tests.cpp /Fo:out\ /Fe:out\stream_tests.exe
if errorlevel 1 exit /b 1
out\stream_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 video_tests.cpp /Fo:out\ /Fe:out\video_tests.exe
if errorlevel 1 exit /b 1
out\video_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 video_queue_tests.cpp /Fo:out\ /Fe:out\video_queue_tests.exe
if errorlevel 1 exit /b 1
out\video_queue_tests.exe
