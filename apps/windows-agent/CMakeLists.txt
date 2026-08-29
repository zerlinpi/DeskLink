cmake_minimum_required(VERSION 3.24)
project(desklink_windows_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release artifacts are intended to be copy-deployable on supported Windows
# machines. Use the static MSVC runtime for this project and all CMake
# FetchContent dependencies so desklink-agent/service do not require a separately
# installed Visual C++ Redistributable merely to start.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

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
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/service_shutdown_bridge.cpp
  src/signal_token_client.cpp
  src/signal_token_client.h
  src/turn_credential_client.cpp
  src/turn_credential_client.h
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
    winhttp
    shell32
    bcrypt
)

add_executable(desklink-service
  src/service_main.cpp
  src/protected_credential_store.cpp
  src/protected_credential_store.h
  src/service_auth_broker.cpp
  src/service_auth_broker.h
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/signal_token_client.cpp
  src/signal_token_client.h
)
target_compile_definitions(desklink-service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-service PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-service
  PRIVATE
    nlohmann_json::nlohmann_json
    advapi32
    crypt32
    shell32
    userenv
    winhttp
    wtsapi32
)

add_executable(desklink-service-auth-smoke
  src/service_auth_broker_smoke.cpp
  src/service_auth_broker.cpp
  src/service_auth_broker.h
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/signal_token_client.cpp
  src/signal_token_client.h
)
target_compile_definitions(desklink-service-auth-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-service-auth-smoke PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-service-auth-smoke
  PRIVATE
    nlohmann_json::nlohmann_json
    advapi32
    shell32
    winhttp
    ws2_32
)

add_executable(desklink-access-proof-smoke
  src/access_proof_smoke.cpp
)
target_compile_definitions(desklink-access-proof-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-access-proof-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-access-proof-smoke PRIVATE bcrypt)
