cmake_minimum_required(VERSION 3.24)
project(desklink_windows_agent LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" DESKLINK_VERSION)
string(STRIP "${DESKLINK_VERSION}" DESKLINK_VERSION)
if(NOT DESKLINK_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9A-Za-z][0-9A-Za-z.-]*))?$")
  message(FATAL_ERROR "../../VERSION must contain semantic major.minor.patch with an optional prerelease suffix")
endif()
set(DESKLINK_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(DESKLINK_VERSION_MINOR "${CMAKE_MATCH_2}")
set(DESKLINK_VERSION_PATCH "${CMAKE_MATCH_3}")

function(configure_desklink_version_resource output_name file_description internal_name original_filename)
  set(DESKLINK_FILE_DESCRIPTION "${file_description}")
  set(DESKLINK_INTERNAL_NAME "${internal_name}")
  set(DESKLINK_ORIGINAL_FILENAME "${original_filename}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.rc"
    @ONLY
  )
endfunction()

configure_desklink_version_resource(
  desklink-agent-version
  "DeskLink Remote Desktop Agent"
  "desklink-agent"
  "desklink-agent.exe"
)
configure_desklink_version_resource(
  desklink-service-version
  "DeskLink Remote Desktop Service"
  "desklink-service"
  "desklink-service.exe"
)
configure_desklink_version_resource(
  desklink-manager-version
  "DeskLink Windows Host Setup"
  "DeskLink"
  "DeskLink.exe"
)
configure_desklink_version_resource(
  desklink-media-probe-version
  "DeskLink GPU Media Pipeline Probe"
  "desklink-media-probe"
  "desklink-media-probe.exe"
)

# Release artifacts are intended to be copy-deployable on supported Windows
# machines. Use the static MSVC runtime for this project and all CMake
# FetchContent dependencies so DeskLink binaries do not require a separately
# installed Visual C++ Redistributable merely to start.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
  # FindOpenSSL honors these hints on MSVC. Prefer both static OpenSSL libraries
  # and their /MT build so libdatachannel does not introduce libssl/libcrypto
  # runtime DLLs or a CRT mismatch into the portable artifact.
  set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
  set(OPENSSL_MSVC_STATIC_RT TRUE CACHE BOOL "" FORCE)
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
  src/clipboard_win32.cpp
  src/clipboard_win32.h
  src/desktop_capture.cpp
  src/desktop_capture.h
  src/file_transfer_receiver.cpp
  src/file_transfer_receiver.h
  src/file_transfer_chunk_policy.h
  src/gpu_color_converter.cpp
  src/gpu_color_converter.h
  src/h264_annexb.cpp
  src/h264_annexb.h
  src/h264_encoder.cpp
  src/h264_encoder.h
  src/host_capabilities.cpp
  src/host_capabilities.h
  src/host_capabilities_publisher.cpp
  src/input_channel_authority.h
  src/input_injector.cpp
  src/input_injector.h
  src/pointer_wire.cpp
  src/pointer_wire.h
  src/performance_tuning.cpp
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/secure_attention.cpp
  src/secure_attention.h
  src/service_shutdown_bridge.cpp
  src/signal_token_client.cpp
  src/signal_token_client.h
  src/turn_credential_client.cpp
  src/turn_credential_client.h
  src/video_policy.cpp
  src/video_policy.h
  src/webrtc_session.cpp
  src/webrtc_session.h
  "${CMAKE_CURRENT_BINARY_DIR}/desklink-agent-version.rc"
)

target_compile_definitions(desklink-agent PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-agent PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-agent
  PRIVATE
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    advapi32
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
  src/active_session.cpp
  src/active_session.h
  src/device_identity.cpp
  src/device_identity.h
  src/protected_credential_store.cpp
  src/protected_credential_store.h
  src/service_auth_broker.cpp
  src/service_auth_broker.h
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/secure_attention.cpp
  src/secure_attention.h
  src/signal_token_client.cpp
  src/signal_token_client.h
  "${CMAKE_CURRENT_BINARY_DIR}/desklink-service-version.rc"
)
target_compile_definitions(desklink-service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-service PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-service
  PRIVATE
    nlohmann_json::nlohmann_json
    advapi32
    bcrypt
    crypt32
    shell32
    userenv
    winhttp
    wtsapi32
)

# User-facing Windows setup/management executable. This is deliberately a native
# Win32 binary with no extra runtime dependency. It installs Agent/Service into
# Program Files, stores the Access Code/device credential with DPAPI, writes
# service-scoped environment settings, and can run local Signal/TURN diagnostics
# without requiring PowerShell or a separate support utility.
add_executable(desklink-manager WIN32
  src/setup_main.cpp
  src/device_identity.cpp
  src/device_identity.h
  src/protected_credential_store.cpp
  src/protected_credential_store.h
  "${CMAKE_CURRENT_BINARY_DIR}/desklink-manager-version.rc"
)
set_target_properties(desklink-manager PROPERTIES OUTPUT_NAME "DeskLink")
target_compile_definitions(desklink-manager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-manager PRIVATE /W4 /permissive-)
target_link_options(desklink-manager PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
target_link_libraries(
  desklink-manager
  PRIVATE
    advapi32
    bcrypt
    comctl32
    crypt32
    shell32
    user32
    winhttp
    ws2_32
)

# Runs the same local media primitives used by the Agent without opening any
# network connection. Keeping this as a separate process makes GPU/driver hangs
# diagnosable without freezing the DeskLink settings UI.
add_executable(desklink-media-probe
  src/media_probe_main.cpp
  src/desktop_capture.cpp
  src/desktop_capture.h
  src/gpu_color_converter.cpp
  src/gpu_color_converter.h
  src/h264_annexb.cpp
  src/h264_annexb.h
  src/h264_encoder.cpp
  src/h264_encoder.h
  "${CMAKE_CURRENT_BINARY_DIR}/desklink-media-probe-version.rc"
)
target_compile_definitions(desklink-media-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-media-probe PRIVATE /W4 /permissive-)
target_link_libraries(
  desklink-media-probe
  PRIVATE
    d3d11
    dxgi
    dxguid
    mfplat
    mf
    mfuuid
    strmiids
    user32
    ole32
)

add_executable(desklink-service-auth-smoke
  src/service_auth_broker_smoke.cpp
  src/service_auth_broker.cpp
  src/service_auth_broker.h
  src/service_auth_client.cpp
  src/service_auth_client.h
  src/secure_attention.cpp
  src/secure_attention.h
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

add_executable(desklink-h264-annexb-smoke
  src/h264_annexb_smoke.cpp
  src/h264_annexb.cpp
  src/h264_annexb.h
)
target_compile_definitions(desklink-h264-annexb-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-h264-annexb-smoke PRIVATE /W4 /permissive-)

add_executable(desklink-video-policy-smoke
  src/video_policy_smoke.cpp
  src/video_policy.cpp
  src/video_policy.h
)
target_compile_definitions(desklink-video-policy-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-video-policy-smoke PRIVATE /W4 /permissive-)

add_executable(desklink-pointer-wire-smoke
  src/pointer_wire_smoke.cpp
  src/pointer_wire.cpp
  src/pointer_wire.h
)
target_compile_definitions(desklink-pointer-wire-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-pointer-wire-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-pointer-wire-smoke PRIVATE nlohmann_json::nlohmann_json)

add_executable(desklink-input-channel-authority-smoke
  src/input_channel_authority_smoke.cpp
  src/input_channel_authority.h
)
target_compile_definitions(desklink-input-channel-authority-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-input-channel-authority-smoke PRIVATE /W4 /permissive-)

add_executable(desklink-file-transfer-chunk-policy-smoke
  src/file_transfer_chunk_policy_smoke.cpp
  src/file_transfer_chunk_policy.h
)
target_compile_definitions(desklink-file-transfer-chunk-policy-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-file-transfer-chunk-policy-smoke PRIVATE /W4 /permissive-)
add_executable(desklink-active-session-smoke
  src/active_session_smoke.cpp
  src/active_session.cpp
  src/active_session.h
)
target_compile_definitions(desklink-active-session-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-active-session-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-active-session-smoke PRIVATE wtsapi32)

add_executable(desklink-device-identity-smoke
  src/device_identity_smoke.cpp
  src/device_identity.cpp
  src/device_identity.h
)
target_compile_definitions(desklink-device-identity-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-device-identity-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-device-identity-smoke PRIVATE advapi32 bcrypt)

add_executable(desklink-secure-attention-smoke
  src/secure_attention_smoke.cpp
  src/secure_attention.cpp
  src/secure_attention.h
)
target_compile_definitions(desklink-secure-attention-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-secure-attention-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-secure-attention-smoke PRIVATE advapi32)

add_executable(desklink-host-capabilities-smoke
  src/host_capabilities_smoke.cpp
  src/host_capabilities.cpp
  src/host_capabilities.h
)
target_compile_definitions(
  desklink-host-capabilities-smoke
  PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DESKLINK_HOST_CAPABILITIES_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../apps/web/src/fixtures/host-capabilities-v1.windows.json"
)
target_compile_options(desklink-host-capabilities-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-host-capabilities-smoke PRIVATE nlohmann_json::nlohmann_json)
