@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
set "taskUiDeps=%TEMP%\lumiri-ui-deps"
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /DLUMIRI_UI_PREVIEW /DSDL_MAIN_HANDLED /I"%taskUiDeps%\include" /I"%taskUiDeps%\include\SDL2" ui_preview.cpp ..\switch-client\source\ui.cpp ..\switch-client\source\menu.cpp /Fo:out\ /Fe:out\ui_preview.exe /link /LIBPATH:"%taskUiDeps%\SDL2-2.28.5\lib\x64" /LIBPATH:"%taskUiDeps%\SDL2_ttf-2.20.2\lib\x64" SDL2.lib SDL2_ttf.lib
if errorlevel 1 exit /b 1
copy /y "%taskUiDeps%\SDL2-2.28.5\lib\x64\SDL2.dll" out\ >nul
copy /y "%taskUiDeps%\SDL2_ttf-2.20.2\lib\x64\*.dll" out\ >nul
out\ui_preview.exe
