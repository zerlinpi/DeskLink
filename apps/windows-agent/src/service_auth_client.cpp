#include "service_auth_client.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace desklink {
namespace {
using namespace std::chrono_literals;

constexpr wchar_t kArgumentPrefix[] = L"--service-auth-pipe=";
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\DeskLink.Auth.";
constexpr size_t kMaxResponseBytes = 64 * 1024;

std::wstring ServiceAuthPipeName() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return {};

  constexpr size_t prefix_length =
      (sizeof(kArgumentPrefix) / sizeof(kArgumentPrefix[0])) - 1;
  std::wstring result;
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument(argv[i]);
    if (argument.rfind(kArgumentPrefix, 0) == 0) {
      result = argument.substr(prefix_length);
      break;
    }
  }
  LocalFree(argv);

  if (result.rfind(kPipePrefix, 0) != 0 || result.size() > 512) return {};
  return result;
}

void SetError(std::string* error, const std::string& value) {
  if (error) *error = value;
}

class LocalHandle {
 public:
  explicit LocalHandle(HANDLE handle) : handle_(handle) {}
  ~LocalHandle() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  LocalHandle(const LocalHandle&) = delete;
  LocalHandle& operator=(const LocalHandle&) = delete;
  HANDLE get() const { return handle_; }
  explicit operator bool() const {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{nullptr};
};

}  // namespace

bool ServiceAuthBrokerConfigured() {
  return !ServiceAuthPipeName().empty();
}

bool FetchServiceBrokerSignalToken(
    RuntimeSignalToken* signal_token,
    std::string* error) {
  if (!signal_token) {
    SetError(error, "signal token output is required");
    return false;
  }
  *signal_token = {};

  const std::wstring pipe_name = ServiceAuthPipeName();
  if (pipe_name.empty()) {
    SetError(error, "service authentication broker is not configured");
    return false;
  }

  HANDLE raw_pipe = INVALID_HANDLE_VALUE;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    raw_pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (raw_pipe != INVALID_HANDLE_VALUE) break;

    const DWORD open_error = GetLastError();
    if (open_error != ERROR_PIPE_BUSY && open_error != ERROR_FILE_NOT_FOUND) {
      SetError(error, "unable to connect to local service authentication broker");
      return false;
    }
    if (open_error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(pipe_name.c_str(), 250);
    } else {
      std::this_thread::sleep_for(100ms);
    }
  }

  LocalHandle pipe(raw_pipe);
  if (!pipe) {
    SetError(error, "local service authentication broker is unavailable");
    return false;
  }

  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
    SetError(error, "unable to configure local authentication pipe");
    return false;
  }

  constexpr char request[] = "signal-token";
  DWORD written = 0;
  if (!WriteFile(
          pipe.get(),
          request,
          static_cast<DWORD>(sizeof(request) - 1),
          &written,
          nullptr) ||
      written != sizeof(request) - 1) {
    SetError(error, "unable to request signal token from local service");
    return false;
  }

  std::string response;
  char buffer[4096];
  while (response.size() < kMaxResponseBytes) {
    DWORD read = 0;
    const BOOL ok = ReadFile(
        pipe.get(),
        buffer,
        static_cast<DWORD>(sizeof(buffer)),
        &read,
        nullptr);
    if (read > 0) response.append(buffer, read);
    if (ok) break;
    if (GetLastError() != ERROR_MORE_DATA) {
      SetError(error, "unable to read signal token from local service");
      return false;
    }
  }
  if (response.empty() || response.size() >= kMaxResponseBytes) {
    SetError(error, "local service authentication response is invalid");
    return false;
  }

  const auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    SetError(error, "local service authentication response is not valid JSON");
    return false;
  }
  if (!json.value("ok", false)) {
    SetError(error, json.value("error", "local service authentication failed"));
    return false;
  }

  RuntimeSignalToken result;
  result.token = json.value("token", "");
  result.expires_at = json.value("expiresAt", int64_t{0});
  if (result.token.empty() || result.expires_at <= 0) {
    SetError(error, "local service authentication response is missing required fields");
    return false;
  }

  *signal_token = std::move(result);
  return true;
}

}  // namespace desklink
