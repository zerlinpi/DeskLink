#include "signal_token_client.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "service_auth_client.h"

namespace desklink {
namespace {

class WinHttpHandle {
 public:
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_) WinHttpCloseHandle(handle_);
  }

  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;

  WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
    if (this == &other) return *this;
    if (handle_) WinHttpCloseHandle(handle_);
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
  }

  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_{nullptr};
};

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (count <= 0) return {};

  std::wstring result(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          count) != count) {
    return {};
  }
  return result;
}

bool IsLocalHost(const std::wstring& host) {
  std::wstring normalized = host;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return normalized == L"localhost" ||
         normalized == L"127.0.0.1" ||
         normalized == L"::1" ||
         normalized == L"[::1]";
}

void SetError(std::string* error, const std::string& value) {
  if (error) *error = value;
}

void SecureWipe(std::wstring* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
  value->clear();
}

}  // namespace

bool FetchRuntimeSignalToken(
    const std::string& endpoint,
    const std::string& device_id,
    const std::string& device_credential,
    RuntimeSignalToken* signal_token,
    std::string* error) {
  if (!signal_token) {
    SetError(error, "signal token output is required");
    return false;
  }
  *signal_token = {};

  // Only reroute signal-token refresh when the Service explicitly enabled that
  // broker capability. A Service Pipe may exist solely to deliver a protected
  // access code, in which case direct/manual signal-token behavior must remain
  // unchanged.
  if (ServiceSignalTokenBrokerConfigured()) {
    return FetchServiceBrokerSignalToken(signal_token, error);
  }

  if (endpoint.empty() || device_id.empty() || device_credential.empty()) {
    SetError(error, "signal token endpoint, device ID and device credential are required");
    return false;
  }

  const std::wstring endpoint_w = Utf8ToWide(endpoint);
  if (endpoint_w.empty()) {
    SetError(error, "signal token endpoint is not valid UTF-8");
    return false;
  }

  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(endpoint_w.c_str(), 0, 0, &components)) {
    SetError(error, "unable to parse signal token endpoint");
    return false;
  }

  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  if (host.empty()) {
    SetError(error, "signal token endpoint has no host");
    return false;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && components.nScheme != INTERNET_SCHEME_HTTP) {
    SetError(error, "signal token endpoint must use HTTPS");
    return false;
  }
  if (!secure && !IsLocalHost(host)) {
    SetError(error, "plain HTTP signal token exchange is allowed only on localhost");
    return false;
  }

  std::wstring path;
  if (components.dwUrlPathLength > 0) {
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (path.empty()) path = L"/";
  if (components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  path += path.find(L'?') == std::wstring::npos ? L"?deviceId=" : L"&deviceId=";
  const std::wstring device_id_w = Utf8ToWide(device_id);
  if (device_id_w.empty()) {
    SetError(error, "device ID is not valid UTF-8");
    return false;
  }
  path += device_id_w;

  WinHttpHandle session(WinHttpOpen(
      L"DeskLink-Agent/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0));
  if (!session) {
    SetError(error, "WinHttpOpen failed");
    return false;
  }
  WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 10000);

  WinHttpHandle connection(WinHttpConnect(
      session.get(),
      host.c_str(),
      components.nPort,
      0));
  if (!connection) {
    SetError(error, "WinHttpConnect failed");
    return false;
  }

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0;
  WinHttpHandle request(WinHttpOpenRequest(
      connection.get(),
      L"GET",
      path.c_str(),
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      request_flags));
  if (!request) {
    SetError(error, "WinHttpOpenRequest failed");
    return false;
  }

  const std::wstring credential_w = Utf8ToWide(device_credential);
  if (credential_w.empty()) {
    SetError(error, "device credential is not valid UTF-8");
    return false;
  }
  const std::wstring headers =
      L"Authorization: Bearer " + credential_w +
      L"\r\nAccept: application/json\r\nCache-Control: no-cache\r\n";

  if (!WinHttpSendRequest(
          request.get(),
          headers.c_str(),
          static_cast<DWORD>(headers.size()),
          WINHTTP_NO_REQUEST_DATA,
          0,
          0,
          0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    SetError(error, "signal token request failed");
    return false;
  }

  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  if (!WinHttpQueryHeaders(
          request.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &status_code,
          &status_size,
          WINHTTP_NO_HEADER_INDEX)) {
    SetError(error, "unable to read signal token response status");
    return false;
  }
  if (status_code != HTTP_STATUS_OK) {
    SetError(error, "signal token endpoint returned HTTP " + std::to_string(status_code));
    return false;
  }

  std::string body;
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      SetError(error, "unable to read signal token response");
      return false;
    }
    if (available == 0) break;
    if (body.size() + available > 64 * 1024) {
      SetError(error, "signal token response is too large");
      return false;
    }

    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), chunk.data(), available, &read)) {
      SetError(error, "unable to read signal token response body");
      return false;
    }
    body.append(chunk.data(), read);
  }

  const auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    SetError(error, "signal token response is not valid JSON");
    return false;
  }

  RuntimeSignalToken result;
  result.token = json.value("token", "");
  result.expires_at = json.value("expiresAt", int64_t{0});
  if (result.token.empty() || result.expires_at <= 0) {
    SetError(error, "signal token response is missing required fields");
    return false;
  }

  *signal_token = std::move(result);
  return true;
}

}  // namespace desklink
