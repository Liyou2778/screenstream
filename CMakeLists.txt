cmake_minimum_required(VERSION 3.20)
project(screenstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(screenstream
    src/main.cpp
    src/common.cpp
    src/thread_pool.cpp
    src/capture.cpp
    src/jpeg_encoder.cpp
    src/protocol.cpp
    src/frame_hub.cpp
    src/server.cpp
    src/page.cpp
)

target_include_directories(screenstream PRIVATE src)

target_compile_definitions(screenstream PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(screenstream PRIVATE
    d3d11
    dxgi
    ws2_32
    windowscodecs
    ole32
    oleaut32
    user32
    gdi32
)

if(MSVC)
    target_compile_options(screenstream PRIVATE /W3 /utf-8 /Zc:__cplusplus /MP)
endif()
