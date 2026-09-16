@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /GL /Gw /Gy /EHsc /MT /DNDEBUG /DUNICODE /D_UNICODE ^
   /I..\third_party\nv-codec-headers\include ^
   /I..\third_party\ViGEmClient\include ^
   main.cpp video.cpp amf_encoder.cpp audio.cpp input.cpp games.cpp ^
   ..\third_party\ViGEmClient\src\ViGEmClient.cpp ^
   /Fo:out\ /Fe:out\lumiri-host.exe ^
   /link /LTCG /OPT:REF /OPT:ICF ws2_32.lib d3d11.lib dxgi.lib d3dcompiler.lib ^
   ole32.lib winmm.lib setupapi.lib user32.lib gdi32.lib qwave.lib avrt.lib
