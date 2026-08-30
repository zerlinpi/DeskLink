#include "service_auth_client.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "secure_attention.h"

namespace desklink {
namespace {
using namespace std::chrono_literals;

constexpr wchar_t kArgumentPrefix[] = L"--service-auth-pipe=";
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\DeskLink.Auth.";
constexpr wchar_t kSignalTokenBrokerFlag[] = L"--service-signal-token-broker";
constexpr wchar_t kAccessCodeBrokerFlag[] = L"--service-access-code-broker";
constexpr wchar_t kSecureAttentionBrokerFlag[] = L"--service-sas-broker";
constexpr size_t kMaxResponseBytes = 64 * 1024;

struct BrokerLaunchOptions {
  std::wstring pipe_name;
  bool signal_token{false};
  bool access_code{false};
  bool secure_attention_sequence{false};
};

BrokerLaunchOptions ServiceBrokerOptions() {
  BrokerLaunchOptions options;
  bool explicit_capability = false;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return options;

  constexpr size_t prefix_length =
      (sizeof(kArgumentPrefix) / sizeof(kArgumentPrefix[0])) - 1;
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument(argv[i]);
    if (argument.rfind(kArgumentPrefix, 0) == 0) {
      options.pipe_name = argument.substr(prefix_length);
    } else if (argument == kSignalTokenBrokerFlag) {
      options.signal_token = true;
      explicit_capability = true;
    } else if (argument == kAccessCodeBrokerFlag) {
      options.access_code = true;
      explicit_capability = true;
    } else if (argument == kSecureAttentionBrokerFlag) {
      options.secure_attention_sequence = true;
      explicit_capability = true;
    }
  }
  LocalFree(argv);

  if (options.pipe_name.rfind(kPipePrefix, 0) != 0 || options.pipe_name.size() > 512) {
    return {};
  }

  // Existing Service builds launched the signal-token broker with only the Pipe
  // argument. Preserve that behavior until all Service builds have migrated to
  // explicit capability flags. Once any capability flag is present, only the
  // explicitly named capabilities are enabled.
  if (!explicit_capability) options.signal_token = true;
  return options;
}

void SetError(std::string* error, const std::string& value) {
  if (error) *error = value;
}

void SecureWipe(std::string* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size());
  value->clear();
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

bool BrokerRequest(
    const std::string& command,
    nlohmann::json* response_json,
    std::string* error) {
  if (!response_json) {
    SetError(error, "local broker response output is required");
    return false;
  }
  *response_json = nlohmann::json{};

  const BrokerLaunchOptions options = ServiceBrokerOptions();
  if (options.pipe_name.empty()) {
    SetError(error, "service authentication broker is not configured");
    return false;
  }

  HANDLE raw_pipe = INVALID_HANDLE_VALUE;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    raw_pipe = CreateFileW(
        options.pipe_name.c_str(),
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
      WaitNamedPipeW(options.pipe_name.c_str(), 250);
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

  const DWORD request_size = static_cast<DWORD>(command.size());
  DWORD written = 0;
  if (!WriteFile(
          pipe.get(),
          command.data(),
          request_size,
          &written,
          nullptr) ||
      written != request_size) {
    SetError(error, "unable to send local authentication request");
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
      SecureWipe(&response);
      SetError(error, "unable to read local authentication response");
      return false;
    }
  }
  if (response.empty() || response.size() >= kMaxResponseBytes) {
    SecureWipe(&response);
    SetError(error, "local service authentication response is invalid");
    return false;
  }

  auto parsed = nlohmann::json::parse(response, nullptr, false);
  SecureWipe(&response);
  if (parsed.is_discarded() || !parsed.is_object()) {
    SetError(error, "local service authentication response is not valid JSON");
    return false;
  }

  *response_json = parsed;
  if (!parsed.value("ok", false)) {
    SetError(error, parsed.value("error", "local service authentication failed"));
    return false;
  }
  return true;
}

}  // namespace

bool ServiceAuthBrokerConfigured() {
  return !ServiceBrokerOptions().pipe_name.empty();
}

bool ServiceSignalTokenBrokerConfigured() {
  const BrokerLaunchOptions options = ServiceBrokerOptions();
  return !options.pipe_name.empty() && options.signal_token;
}

bool ServiceAccessCodeBrokerConfigured() {
  const BrokerLaunchOptions options = ServiceBrokerOptions();
  return !options.pipe_name.empty() && options.access_code;
}

bool ServiceSecureAttentionBrokerConfigured() {
  const BrokerLaunchOptions options = ServiceBrokerOptions();
  return !options.pipe_name.empty() && options.secure_attention_sequence;
}

bool FetchServiceSecureAttentionStatus(
    ServiceSecureAttentionStatus* status,
    std::string* error) {
  if (!status) {
    SetError(error, "secure-attention status output is required");
    return false;
  }
  if (error) error->clear();
  *status = {};
  status->broker_configured = ServiceSecureAttentionBrokerConfigured();

  std::wstring capability_error;
  const SecureAttentionCapability capability =
      QuerySecureAttentionCapability(&capability_error);
  status->api_available = capability.api_available;
  status->policy_readable = capability.policy_error == ERROR_SUCCESS;
  status->policy_allows_services = capability.policy_allows_services;
  status->policy = SecureAttentionPolicyCode(capability.policy);
  status->available = status->broker_configured &&
                      status->api_available &&
                      status->policy_readable &&
                      status->policy_allows_services;

  if (error && !status->available) {
    if (!status->broker_configured) {
      *error = "service secure-attention broker is not enabled";
    } else if (!status->api_available) {
      *error = "Windows Secure Attention Sequence API is unavailable";
    } else if (!status->policy_readable) {
      *error = "Windows SoftwareSASGeneration policy could not be read";
    } else if (!status->policy_allows_services) {
      *error = "Windows SoftwareSASGeneration policy does not allow Services";
    }
  }
  return true;
}

bool RequestServiceSecureAttentionSequence(
    std::string* error,
    std::string* error_code) {
  if (error_code) error_code->clear();

  ServiceSecureAttentionStatus status;
  std::string status_error;
  if (!FetchServiceSecureAttentionStatus(&status, &status_error)) {
    SetError(error, status_error.empty() ? "secure-attention status unavailable" : status_error);
    if (error_code) *error_code = "capability-unavailable";
    return false;
  }
  if (!status.broker_configured) {
    SetError(error, status_error);
    if (error_code) *error_code = "service-broker-unavailable";
    return false;
  }
  if (!status.api_available) {
    SetError(error, status_error);
    if (error_code) *error_code = "api-unavailable";
    return false;
  }
  if (!status.policy_readable) {
    SetError(error, status_error);
    if (error_code) *error_code = "policy-read-error";
    return false;
  }
  if (!status.policy_allows_services) {
    SetError(error, status_error);
    if (error_code) *error_code = "policy-not-allowed";
    return false;
  }

  nlohmann::json response;
  if (BrokerRequest("secure-attention-sequence", &response, error)) return true;

  const std::string broker_error = response.value("error", "");
  if (error_code) {
    if (broker_error.find("rate limited") != std::string::npos) {
      *error_code = "rate-limited";
    } else if (broker_error.find("capability is not enabled") != std::string::npos) {
      *error_code = "service-broker-unavailable";
    } else {
      *error_code = "dispatch-failed";
    }
  }
  return false;
}

bool FetchServiceBrokerSignalToken(
    RuntimeSignalToken* signal_token,
    std::string* error) {
  if (!signal_token) {
    SetError(error, "signal token output is required");
    return false;
  }
  *signal_token = {};
  if (!ServiceSignalTokenBrokerConfigured()) {
    SetError(error, "service signal-token broker is not enabled");
    return false;
  }

  nlohmann::json response;
  if (!BrokerRequest("signal-token", &response, error)) return false;

  RuntimeSignalToken result;
  result.token = response.value("token", "");
  result.expires_at = response.value("expiresAt", int64_t{0});
  if (result.token.empty() || result.expires_at <= 0) {
    SetError(error, "local service signal-token response is missing required fields");
    return false;
  }

  *signal_token = std::move(result);
  return true;
}

bool FetchServiceBrokerAccessCode(
    std::string* access_code,
    std::string* error) {
  if (!access_code) {
    SetError(error, "access code output is required");
    return false;
  }
  access_code->clear();
  if (!ServiceAccessCodeBrokerConfigured()) {
    SetError(error, "service access-code broker is not enabled");
    return false;
  }

  nlohmann::json response;
  if (!BrokerRequest("access-code", &response, error)) return false;

  std::string result = response.value("accessCode", "");
  if (result.size() < 8 || result.size() > 1024) {
    SecureWipe(&result);
    SetError(error, "local service access-code response is invalid");
    return false;
  }

  *access_code = std::move(result);
  return true;
}

}  // namespace desklink
