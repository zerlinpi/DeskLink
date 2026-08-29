#include "service_auth_broker.h"

#include <windows.h>
#include <sddl.h>

#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "signal_token_client.h"

namespace desklink {
namespace {

constexpr DWORD kPipeBufferBytes = 16 * 1024;
constexpr DWORD kMaxRequestBytes = 128;
constexpr int64_t kTokenReuseSafetySeconds = 90;

std::mutex g_broker_mutex;
std::jthread g_broker_thread;
std::wstring g_broker_pipe_name;

void SecureWipe(std::string* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size());
  value->clear();
}

void WipeToken(RuntimeSignalToken* token) {
  if (!token) return;
  SecureWipe(&token->token);
  token->expires_at = 0;
}

bool TokenFreshEnough(const RuntimeSignalToken& token) {
  if (token.token.empty() || token.expires_at <= 0) return false;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  return token.expires_at > now + kTokenReuseSafetySeconds;
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
  LocalHandle& operator=(LocalHandle&& other) noexcept {
    if (this == &other) return *this;
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
  }

  HANDLE get() const { return handle_; }
  HANDLE release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }
  explicit operator bool() const {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{nullptr};
};

bool ProcessUserSidString(
    uint32_t process_id,
    std::wstring* sid_string,
    std::wstring* error) {
  if (!sid_string || process_id == 0) {
    SetError(error, L"Invalid Agent process for local authentication ACL");
    return false;
  }
  sid_string->clear();

  LocalHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process) {
    SetError(error, L"Unable to open Agent process for local authentication ACL", GetLastError());
    return false;
  }

  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) {
    SetError(error, L"Unable to read Agent process token", GetLastError());
    return false;
  }
  LocalHandle token(raw_token);

  DWORD required = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
  if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    SetError(error, L"Unable to size Agent user token information", GetLastError());
    return false;
  }

  std::vector<BYTE> token_buffer(required);
  if (!GetTokenInformation(
          token.get(),
          TokenUser,
          token_buffer.data(),
          required,
          &required)) {
    SetError(error, L"Unable to read Agent user token information", GetLastError());
    return false;
  }

  const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
  LPWSTR raw_sid = nullptr;
  if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid) || !raw_sid) {
    SetError(error, L"Unable to format Agent user SID", GetLastError());
    return false;
  }
  *sid_string = raw_sid;
  LocalFree(raw_sid);
  return !sid_string->empty();
}

LocalHandle CreateBrokerPipe(
    const std::wstring& pipe_name,
    const std::wstring& client_sid,
    bool first_instance,
    std::wstring* error = nullptr) {
  if (client_sid.empty()) {
    SetError(error, L"Agent user SID is required for local authentication pipe");
    return {};
  }

  const std::wstring pipe_dacl =
      L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + client_sid + L")";

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          pipe_dacl.c_str(),
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
    SetError(error, L"Unable to create local authentication pipe security descriptor", GetLastError());
    return {};
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  DWORD open_mode = PIPE_ACCESS_DUPLEX;
  if (first_instance) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

  SetLastError(ERROR_SUCCESS);
  HANDLE pipe = CreateNamedPipeW(
      pipe_name.c_str(),
      open_mode,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,
      kPipeBufferBytes,
      kPipeBufferBytes,
      5000,
      &attributes);
  const DWORD create_error = GetLastError();
  LocalFree(descriptor);

  if (pipe == INVALID_HANDLE_VALUE) {
    SetError(error, L"Unable to create local authentication pipe", create_error);
    return {};
  }
  return LocalHandle(pipe);
}

bool WriteRawResponse(HANDLE pipe, std::string* payload) {
  if (!payload || payload->empty() || payload->size() > kPipeBufferBytes) {
    SecureWipe(payload);
    return false;
  }
  const DWORD size = static_cast<DWORD>(payload->size());
  DWORD written = 0;
  const bool ok = WriteFile(pipe, payload->data(), size, &written, nullptr) && written == size;
  SecureWipe(payload);
  return ok;
}

bool WriteJsonResponse(HANDLE pipe, const nlohmann::json& response) {
  std::string payload = response.dump();
  return WriteRawResponse(pipe, &payload);
}

void AppendJsonEscapedString(const std::string& value, std::string* output) {
  static constexpr char kHex[] = "0123456789abcdef";
  output->push_back('"');
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        output->append("\\\"");
        break;
      case '\\':
        output->append("\\\\");
        break;
      case '\b':
        output->append("\\b");
        break;
      case '\f':
        output->append("\\f");
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      default:
        if (ch < 0x20) {
          output->append("\\u00");
          output->push_back(kHex[(ch >> 4) & 0x0F]);
          output->push_back(kHex[ch & 0x0F]);
        } else {
          output->push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  output->push_back('"');
}

bool WriteAccessCodeResponse(HANDLE pipe, const std::string& access_code) {
  std::string payload = "{\"ok\":true,\"accessCode\":";
  payload.reserve(payload.size() + access_code.size() + 4);
  AppendJsonEscapedString(access_code, &payload);
  payload.push_back('}');
  return WriteRawResponse(pipe, &payload);
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
    std::wstring client_sid,
    HANDLE initial_pipe,
    uint32_t expected_client_pid,
    std::string endpoint,
    std::string device_id,
    std::string device_credential,
    std::string access_code) {
  LocalHandle ready_pipe(initial_pipe);
  RuntimeSignalToken cached_token;
  const bool signal_enabled = !endpoint.empty() && !device_id.empty() && !device_credential.empty();
  const bool access_code_enabled = !access_code.empty();

  while (!stop_token.stop_requested()) {
    LocalHandle pipe;
    if (ready_pipe) {
      pipe = std::move(ready_pipe);
    } else {
      pipe = CreateBrokerPipe(pipe_name, client_sid, false);
      if (!pipe) break;
    }

    const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr)
        ? TRUE
        : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) continue;
    if (stop_token.stop_requested()) break;

    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &client_pid) ||
        client_pid != expected_client_pid) {
      WriteJsonResponse(
          pipe.get(),
          nlohmann::json{{"ok", false}, {"error", "unauthorized local client"}});
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
    if (command == "access-code") {
      if (!access_code_enabled) {
        WriteJsonResponse(
            pipe.get(),
            nlohmann::json{{"ok", false}, {"error", "access-code capability is not enabled"}});
      } else {
        WriteAccessCodeResponse(pipe.get(), access_code);
      }
      FlushFileBuffers(pipe.get());
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    if (command != "signal-token") {
      WriteJsonResponse(
          pipe.get(),
          nlohmann::json{{"ok", false}, {"error", "unsupported request"}});
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    if (!signal_enabled) {
      WriteJsonResponse(
          pipe.get(),
          nlohmann::json{{"ok", false}, {"error", "signal-token capability is not enabled"}});
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    if (!TokenFreshEnough(cached_token)) {
      RuntimeSignalToken fresh_token;
      std::string fetch_error;
      if (!FetchRuntimeSignalToken(
              endpoint,
              device_id,
              device_credential,
              &fresh_token,
              &fetch_error)) {
        WriteJsonResponse(
            pipe.get(),
            nlohmann::json{{"ok", false}, {"error", "signal token exchange failed: " + fetch_error}});
        DisconnectNamedPipe(pipe.get());
        continue;
      }
      WipeToken(&cached_token);
      cached_token = std::move(fresh_token);
    }

    WriteJsonResponse(
        pipe.get(),
        nlohmann::json{
            {"ok", true},
            {"token", cached_token.token},
            {"expiresAt", cached_token.expires_at},
        });
    FlushFileBuffers(pipe.get());
    DisconnectNamedPipe(pipe.get());
  }

  WipeToken(&cached_token);
  SecureWipe(&device_credential);
  SecureWipe(&access_code);
}

}  // namespace

bool StartServiceAuthBroker(
    const std::wstring& pipe_name,
    uint32_t expected_client_pid,
    const std::string& signal_token_endpoint,
    const std::string& device_id,
    std::string device_credential,
    std::string access_code,
    std::wstring* error) {
  const bool any_signal_field = !signal_token_endpoint.empty() ||
                                !device_id.empty() ||
                                !device_credential.empty();
  const bool signal_enabled = !signal_token_endpoint.empty() &&
                              !device_id.empty() &&
                              !device_credential.empty();
  const bool access_code_enabled = !access_code.empty();
  if (pipe_name.rfind(L"\\\\.\\pipe\\DeskLink.Auth.", 0) != 0 ||
      expected_client_pid == 0 ||
      (any_signal_field && !signal_enabled) ||
      (!signal_enabled && !access_code_enabled)) {
    SecureWipe(&device_credential);
    SecureWipe(&access_code);
    SetError(error, L"Invalid local authentication broker configuration");
    return false;
  }

  StopServiceAuthBroker();

  std::wstring client_sid;
  if (!ProcessUserSidString(expected_client_pid, &client_sid, error)) {
    SecureWipe(&device_credential);
    SecureWipe(&access_code);
    return false;
  }

  LocalHandle first_pipe = CreateBrokerPipe(pipe_name, client_sid, true, error);
  if (!first_pipe) {
    SecureWipe(&device_credential);
    SecureWipe(&access_code);
    return false;
  }

  HANDLE first_pipe_handle = first_pipe.release();
  try {
    std::scoped_lock lock(g_broker_mutex);
    g_broker_pipe_name = pipe_name;
    g_broker_thread = std::jthread(
        BrokerLoop,
        pipe_name,
        client_sid,
        first_pipe_handle,
        expected_client_pid,
        signal_token_endpoint,
        device_id,
        std::move(device_credential),
        std::move(access_code));
  } catch (...) {
    CloseHandle(first_pipe_handle);
    SecureWipe(&device_credential);
    SecureWipe(&access_code);
    SetError(error, L"Unable to start local authentication broker worker");
    return false;
  }
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
