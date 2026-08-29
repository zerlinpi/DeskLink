#include "service_auth_broker.h"

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "signal_token_client.h"

namespace desklink {
namespace {

constexpr wchar_t kPipeDacl[] = L"D:P(A;;GA;;;SY)(A;;GRGW;;;AU)";
constexpr DWORD kPipeBufferBytes = 16 * 1024;
constexpr DWORD kMaxRequestBytes = 128;

std::mutex g_broker_mutex;
std::jthread g_broker_thread;
std::wstring g_broker_pipe_name;

void SecureWipe(std::string* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size());
  value->clear();
}

void SetError(std::wstring* error, const std::wstring& message, DWORD code = ERROR_SUCCESS) {
  if (!error) return;
  *error = message;
  if (code != ERROR_SUCCESS) {
    *error += L" (Win32 ";
    *error += std::to_wstring(code);
    *error += L")";
  }
}

class LocalHandle {
 public:
  LocalHandle() = default;
  explicit LocalHandle(HANDLE handle) : handle_(handle) {}
  ~LocalHandle() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  LocalHandle(const LocalHandle&) = delete;
  LocalHandle& operator=(const LocalHandle&) = delete;
  LocalHandle(LocalHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  HANDLE get() const { return handle_; }
  explicit operator bool() const {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{nullptr};
};

bool WriteJsonResponse(HANDLE pipe, const nlohmann::json& response) {
  const std::string payload = response.dump();
  if (payload.empty() || payload.size() > kPipeBufferBytes) return false;
  DWORD written = 0;
  return WriteFile(
             pipe,
             payload.data(),
             static_cast<DWORD>(payload.size()),
             &written,
             nullptr) &&
         written == payload.size();
}

void WakeBroker(const std::wstring& pipe_name) {
  if (pipe_name.empty()) return;
  HANDLE pipe = CreateFileW(
      pipe_name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

void BrokerLoop(
    std::stop_token stop_token,
    std::wstring pipe_name,
    uint32_t expected_client_pid,
    std::string endpoint,
    std::string device_id,
    std::string device_credential) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeDacl,
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
    SecureWipe(&device_credential);
    return;
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  while (!stop_token.stop_requested()) {
    LocalHandle pipe(CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        kPipeBufferBytes,
        kPipeBufferBytes,
        5000,
        &attributes));
    if (!pipe) break;

    const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr)
        ? TRUE
        : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) continue;
    if (stop_token.stop_requested()) break;

    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &client_pid) ||
        client_pid != expected_client_pid) {
      WriteJsonResponse(pipe.get(), nlohmann::json{{"ok", false}, {"error", "unauthorized local client"}});
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    char request[kMaxRequestBytes]{};
    DWORD read = 0;
    if (!ReadFile(pipe.get(), request, sizeof(request), &read, nullptr) || read == 0) {
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    const std::string command(request, read);
    if (command != "signal-token") {
      WriteJsonResponse(pipe.get(), nlohmann::json{{"ok", false}, {"error", "unsupported request"}});
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    RuntimeSignalToken token;
    std::string fetch_error;
    if (!FetchRuntimeSignalToken(
            endpoint,
            device_id,
            device_credential,
            &token,
            &fetch_error)) {
      WriteJsonResponse(
          pipe.get(),
          nlohmann::json{{"ok", false}, {"error", "signal token exchange failed: " + fetch_error}});
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    WriteJsonResponse(
        pipe.get(),
        nlohmann::json{
            {"ok", true},
            {"token", token.token},
            {"expiresAt", token.expires_at},
        });
    FlushFileBuffers(pipe.get());
    DisconnectNamedPipe(pipe.get());
  }

  LocalFree(descriptor);
  SecureWipe(&device_credential);
}

}  // namespace

bool StartServiceAuthBroker(
    const std::wstring& pipe_name,
    uint32_t expected_client_pid,
    const std::string& signal_token_endpoint,
    const std::string& device_id,
    std::string device_credential,
    std::wstring* error) {
  if (pipe_name.rfind(L"\\\\.\\pipe\\DeskLink.Auth.", 0) != 0 ||
      expected_client_pid == 0 || signal_token_endpoint.empty() ||
      device_id.empty() || device_credential.empty()) {
    SecureWipe(&device_credential);
    SetError(error, L"Invalid local authentication broker configuration");
    return false;
  }

  StopServiceAuthBroker();

  std::scoped_lock lock(g_broker_mutex);
  g_broker_pipe_name = pipe_name;
  g_broker_thread = std::jthread(
      BrokerLoop,
      pipe_name,
      expected_client_pid,
      signal_token_endpoint,
      device_id,
      std::move(device_credential));
  return true;
}

void StopServiceAuthBroker() {
  std::jthread worker;
  std::wstring pipe_name;
  {
    std::scoped_lock lock(g_broker_mutex);
    if (!g_broker_thread.joinable()) {
      g_broker_pipe_name.clear();
      return;
    }
    g_broker_thread.request_stop();
    pipe_name = g_broker_pipe_name;
    worker = std::move(g_broker_thread);
    g_broker_pipe_name.clear();
  }

  WakeBroker(pipe_name);
  if (worker.joinable()) worker.join();
}

}  // namespace desklink
