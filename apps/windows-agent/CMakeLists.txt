cmake_minimum_required(VERSION 3.24)
project(desklink_windows_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Keep the native host self-contained. libdatachannel supplies WebRTC media,
# DataChannels and WebSocket signaling without pulling Chromium/libwebrtc.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.5
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libdatachannel)

add_executable(desklink-agent
  src/main.cpp
  src/desktop_capture.cpp
  src/desktop_capture.h
  src/gpu_color_converter.cpp
  src/gpu_color_converter.h
  src/h264_encoder.cpp
  src/h264_encoder.h
  src/input_injector.cpp
  src/input_injector.h
  src/performance_tuning.cpp
  src/webrtc_session.cpp
  src/webrtc_session.h
)

target_compile_definitions(desklink-agent PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-agent PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-agent
  PRIVATE
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    dxguid
    mfplat
    mf
    mfuuid
    strmiids
    user32
    ole32
    avrt
    winmm
)
