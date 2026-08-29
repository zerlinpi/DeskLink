cmake_minimum_required(VERSION 3.24)
project(desklink_windows_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(desklink-agent
  src/main.cpp
  src/desktop_capture.cpp
  src/desktop_capture.h
  src/input_injector.cpp
  src/input_injector.h
)

target_compile_definitions(desklink-agent PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-agent PRIVATE /W4 /permissive-)
target_link_libraries(desklink-agent PRIVATE d3d11 dxgi mfplat mf mfuuid user32 ole32)
