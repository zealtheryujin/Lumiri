cmake_minimum_required(VERSION 3.20)
project(LumiriHost CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(lumiri-host
    main.cpp
    video.cpp
    amf_encoder.cpp
    audio.cpp
    input.cpp
    games.cpp
    ../third_party/ViGEmClient/src/ViGEmClient.cpp
)

target_include_directories(lumiri-host PRIVATE
    ../third_party/nv-codec-headers/include
    ../third_party/ViGEmClient/include
)

target_compile_definitions(lumiri-host PRIVATE UNICODE _UNICODE
    $<$<CONFIG:Release>:NDEBUG>
)
target_compile_options(lumiri-host PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2 /GL /Gw /Gy>
)
target_link_options(lumiri-host PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG /OPT:REF /OPT:ICF>
)
set_property(TARGET lumiri-host PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
target_link_libraries(lumiri-host PRIVATE
    ws2_32 d3d11 dxgi d3dcompiler ole32 winmm setupapi user32 gdi32 qwave avrt
)
