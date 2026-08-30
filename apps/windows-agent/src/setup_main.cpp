#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "protected_credential_store.h"

namespace {

constexpr wchar_t kWindowClass[] = L"DeskLinkSetupWindow";
constexpr wchar_t kServiceName[] = L"DeskLink";
constexpr wchar_t kServiceRegistryPath[] = L"SYSTEM\\CurrentControlSet\\Services\\DeskLink";

constexpr int kSignalUrl = 1001;
constexpr int kDeviceId = 1002;
constexpr int kAccessCode = 1003;
constexpr int kStunUrl = 1004;
constexpr int kTurnHost = 1005;
constexpr int kTurnPort = 1006;
constexpr int kSignalTokenUrl = 1007;
constexpr int kTurnCredentialsUrl = 1008;
constexpr int kDeviceCredential = 1009;
constexpr int kInstallButton = 1010;
constexpr int kRefreshButton = 1011;
constexpr int kStatusText = 1012;
constexpr int kDiagnoseButton = 1013;
constexpr int kDiagnosticText = 1014;
constexpr int kCopyDeviceButton = 1015;
constexpr int kCopyDiagnosticsButton = 1016;
constexpr int kFps = 1017;

HFONT g_font = nullptr;
HFONT g_title_font = nullptr;
std::wstring g_last_diagnostics;

std::wstring LastErrorMessage(DWORD code) {
  wchar_t* raw = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(
      flags,
      nullptr,
      code,
      0,
      reinterpret_cast<LPWSTR>(&raw),
      0,
      nullptr);
  std::wstring text = length && raw ? std::wstring(raw, length) : L"Win32 error " + std::to_wstring(code);
  if (raw) LocalFree(raw);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
    text.pop_back();
  }
  return text;
}

std::filesystem::path CurrentExecutablePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::wstring ProgramFilesPath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetEnvironmentVariableW(
      L"ProgramFiles",
      buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return L"C:\\Program Files";
  return std::wstring(buffer.data(), length);
}

std::filesystem::path InstallDirectory() {
  return std::filesystem::path(ProgramFilesPath()) / L"DeskLink";
}

std::filesystem::path MediaProbePath() {
  const auto installed = InstallDirectory() / L"desklink-media-probe.exe";
  if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES) return installed;
  return CurrentExecutablePath().parent_path() / L"desklink-media-probe.exe";
}

std::wstring GetControlText(HWND parent, int id) {
  HWND control = GetDlgItem(parent, id);
  if (!control) return {};
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};

  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(control, text.data(), length + 1);
  if (copied <= 0) return {};
  text.resize(static_cast<size_t>(copied));
  return text;
}

void SetControlText(HWND parent, int id, const std::wstring& value) {
  if (HWND control = GetDlgItem(parent, id)) SetWindowTextW(control, value.c_str());
}

void SecureClear(std::wstring* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
  value->clear();
}

bool ValidDeviceId(const std::wstring& value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
  });
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::wstring DefaultDeviceId() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = static_cast<DWORD>(std::size(name));
  if (GetComputerNameW(name, &size) && size > 0) {
    std::wstring id = L"win-";
    id.append(name, size);
    for (wchar_t& ch : id) {
      if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.')) {
        ch = L'-';
      }
    }
    return id;
  }
  return L"windows-host";
}

std::wstring SignalHealthUrl(const std::wstring& signal_url) {
  std::wstring health;
  if (StartsWith(signal_url, L"wss://")) {
    health = L"https://" + signal_url.substr(6);
  } else if (StartsWith(signal_url, L"ws://")) {
    health = L"http://" + signal_url.substr(5);
  } else {
    return {};
  }

  const size_t scheme = health.find(L"://");
  const size_t authority_start = scheme == std::wstring::npos ? 0 : scheme + 3;
  const size_t path = health.find(L'/', authority_start);
  if (path == std::wstring::npos) return health + L"/healthz";
  return health.substr(0, path) + L"/healthz";
}

bool HttpHealthCheck(const std::wstring& url, std::wstring* detail) {
  URL_COMPONENTSW components{};
  components.dwStructSize = sizeof(components);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
    if (detail) *detail = L"无法解析健康检查地址：" + LastErrorMessage(GetLastError());
    return false;
  }

  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path = components.dwUrlPathLength > 0
      ? std::wstring(components.lpszUrlPath, components.dwUrlPathLength)
      : L"/healthz";
  if (components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }

  HINTERNET session = WinHttpOpen(
      L"DeskLink Diagnostics/1.0",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0);
  if (!session) {
    if (detail) *detail = L"WinHTTP 初始化失败：" + LastErrorMessage(GetLastError());
    return false;
  }
  WinHttpSetTimeouts(session, 2500, 2500, 2500, 3500);

  HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (!connection) {
    if (detail) *detail = L"无法连接 Signal 主机：" + LastErrorMessage(GetLastError());
    WinHttpCloseHandle(session);
    return false;
  }

  const DWORD request_flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(
      connection,
      L"GET",
      path.c_str(),
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      request_flags);
  if (!request) {
    if (detail) *detail = L"无法创建健康检查请求：" + LastErrorMessage(GetLastError());
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  bool ok = false;
  if (WinHttpSendRequest(
          request,
          WINHTTP_NO_ADDITIONAL_HEADERS,
          0,
          WINHTTP_NO_REQUEST_DATA,
          0,
          0,
          0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
      ok = status >= 200 && status < 300;
      if (detail) *detail = L"HTTP " + std::to_wstring(status) + L" · " + url;
    }
  } else if (detail) {
    *detail = L"Signal 健康检查失败：" + LastErrorMessage(GetLastError());
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

bool TestTcpEndpoint(
    const std::wstring& host,
    const std::wstring& port,
    std::wstring* detail) {
  WSADATA winsock{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    if (detail) *detail = L"Winsock 初始化失败";
    return false;
  }

  ADDRINFOW hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  ADDRINFOW* addresses = nullptr;
  const int resolve = GetAddrInfoW(host.c_str(), port.c_str(), &hints, &addresses);
  if (resolve != 0 || !addresses) {
    if (detail) *detail = L"TURN 主机 DNS 解析失败（" + std::to_wstring(resolve) + L"）";
    WSACleanup();
    return false;
  }

  bool connected = false;
  int last_error = 0;
  for (ADDRINFOW* address = addresses; address && !connected; address = address->ai_next) {
    SOCKET socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_handle == INVALID_SOCKET) {
      last_error = WSAGetLastError();
      continue;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
      last_error = WSAGetLastError();
      closesocket(socket_handle);
      continue;
    }

    const int connect_result = connect(
        socket_handle,
        address->ai_addr,
        static_cast<int>(address->ai_addrlen));
    if (connect_result == 0) {
      connected = true;
    } else {
      last_error = WSAGetLastError();
      if (last_error == WSAEWOULDBLOCK || last_error == WSAEINPROGRESS) {
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket_handle, &writable);
        FD_SET(socket_handle, &failed);
        timeval timeout{};
        timeout.tv_sec = 2;
        const int selected = select(0, nullptr, &writable, &failed, &timeout);
        if (selected > 0 && FD_ISSET(socket_handle, &writable)) {
          int socket_error = 0;
          int socket_error_size = sizeof(socket_error);
          if (getsockopt(
                  socket_handle,
                  SOL_SOCKET,
                  SO_ERROR,
                  reinterpret_cast<char*>(&socket_error),
                  &socket_error_size) == 0 &&
              socket_error == 0) {
            connected = true;
          } else {
            last_error = socket_error;
          }
        } else if (selected == 0) {
          last_error = WSAETIMEDOUT;
        } else if (selected == SOCKET_ERROR) {
          last_error = WSAGetLastError();
        }
      }
    }
    closesocket(socket_handle);
  }

  FreeAddrInfoW(addresses);
  WSACleanup();
  if (detail) {
    if (connected) {
      *detail = host + L":" + port + L" TCP 可达";
    } else {
      *detail = host + L":" + port + L" TCP 不可达（Winsock " + std::to_wstring(last_error) + L"）";
    }
  }
  return connected;
}

bool CopyTextToClipboard(HWND window, const std::wstring& text) {
  if (text.empty() || !OpenClipboard(window)) return false;
  EmptyClipboard();
  const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    CloseClipboard();
    return false;
  }
  void* destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  memcpy(destination, text.c_str(), bytes);
  GlobalUnlock(memory);
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  return true;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  std::error_code error;
  const auto left_abs = std::filesystem::weakly_canonical(left, error);
  if (error) return false;
  error.clear();
  const auto right_abs = std::filesystem::weakly_canonical(right, error);
  if (error) return false;
  return _wcsicmp(left_abs.c_str(), right_abs.c_str()) == 0;
}

bool CopyBinary(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::wstring* error) {
  if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (error) *error = L"安装包缺少文件：" + source.filename().wstring();
    return false;
  }
  if (SamePath(source, destination)) return true;
  if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
    if (error) {
      *error = L"复制 " + source.filename().wstring() + L" 失败：" + LastErrorMessage(GetLastError());
    }
    return false;
  }
  return true;
}

bool PrepareInstallDirectory(std::filesystem::path* service_path, std::wstring* error) {
  const auto source_dir = CurrentExecutablePath().parent_path();
  const auto install_dir = InstallDirectory();
  if (!CreateDirectoryW(install_dir.c_str(), nullptr)) {
    const DWORD create_error = GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) {
      if (error) *error = L"无法创建安装目录：" + LastErrorMessage(create_error);
      return false;
    }
  }

  const std::vector<std::pair<std::wstring, std::wstring>> files = {
      {L"desklink-agent.exe", L"desklink-agent.exe"},
      {L"desklink-service.exe", L"desklink-service.exe"},
      {L"desklink-media-probe.exe", L"desklink-media-probe.exe"},
      {CurrentExecutablePath().filename().wstring(), L"DeskLink.exe"},
  };
  for (const auto& [source_name, destination_name] : files) {
    if (!CopyBinary(source_dir / source_name, install_dir / destination_name, error)) return false;
  }
  if (service_path) *service_path = install_dir / L"desklink-service.exe";
  return true;
}

bool RunProcessAndWait(
    const std::filesystem::path& executable,
    const std::wstring& arguments,
    DWORD* exit_code,
    std::wstring* error,
    DWORD timeout_ms = 30000) {
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  if (!arguments.empty()) command += L" " + arguments;
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(
          executable.c_str(),
          command_line.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_NO_WINDOW,
          nullptr,
          executable.parent_path().c_str(),
          &startup,
          &process)) {
    if (error) *error = L"启动程序失败：" + LastErrorMessage(GetLastError());
    return false;
  }

  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    if (error) *error = L"程序执行超时。";
    return false;
  }

  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  if (exit_code) *exit_code = code;
  return true;
}

std::wstring MediaProbeFailureText(DWORD code) {
  switch (code) {
    case 2:
      return L"媒体探针参数错误";
    case 10:
      return L"COM 初始化失败";
    case 11:
      return L"Media Foundation 初始化失败";
    case 20:
      return L"无法创建 D3D11 硬件设备；请更新显卡驱动并确认当前会话可使用 GPU";
    case 30:
      return L"DXGI Desktop Duplication 初始化失败；请确认当前为交互式桌面会话并更新显示驱动";
    case 31:
      return L"显示器返回了无效分辨率";
    case 40:
      return L"GPU 不支持当前 BGRA→NV12 视频处理路径";
    case 41:
      return L"桌面帧与 GPU 测试纹理均无法创建";
    case 42:
      return L"GPU BGRA→NV12 转换失败";
    case 50:
      return L"未找到兼容的 Media Foundation 硬件 H.264 编码器";
    case 51:
      return L"硬件 H.264 编码器已初始化，但未能输出视频帧";
    default:
      return L"媒体管线探针失败，退出代码 " + std::to_wstring(code);
  }
}

std::unordered_map<std::wstring, std::wstring> ReadServiceEnvironment() {
  std::unordered_map<std::wstring, std::wstring> result;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServiceRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return result;
  }

  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, L"Environment", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_MULTI_SZ || bytes < sizeof(wchar_t)) {
    RegCloseKey(key);
    return result;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
  if (RegQueryValueExW(
          key,
          L"Environment",
          nullptr,
          nullptr,
          reinterpret_cast<LPBYTE>(buffer.data()),
          &bytes) != ERROR_SUCCESS) {
    RegCloseKey(key);
    return result;
  }
  RegCloseKey(key);

  const wchar_t* cursor = buffer.data();
  while (*cursor) {
    std::wstring entry(cursor);
    const size_t separator = entry.find(L'=');
    if (separator != std::wstring::npos && separator > 0) {
      result.emplace(entry.substr(0, separator), entry.substr(separator + 1));
    }
    cursor += entry.size() + 1;
  }
  return result;
}

bool WriteServiceEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>>& settings,
    std::wstring* error) {
  std::vector<wchar_t> buffer;
  for (const auto& [name, value] : settings) {
    if (value.empty()) continue;
    const std::wstring entry = name + L"=" + value;
    buffer.insert(buffer.end(), entry.begin(), entry.end());
    buffer.push_back(L'\0');
  }
  buffer.push_back(L'\0');

  HKEY key = nullptr;
  const LONG open = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      kServiceRegistryPath,
      0,
      KEY_SET_VALUE,
      &key);
  if (open != ERROR_SUCCESS) {
    if (error) *error = L"无法写入 DeskLink 服务配置：" + LastErrorMessage(static_cast<DWORD>(open));
    return false;
  }
  const LONG set = RegSetValueExW(
      key,
      L"Environment",
      0,
      REG_MULTI_SZ,
      reinterpret_cast<const BYTE*>(buffer.data()),
      static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
  RegCloseKey(key);
  if (set != ERROR_SUCCESS) {
    if (error) *error = L"保存 DeskLink 服务配置失败：" + LastErrorMessage(static_cast<DWORD>(set));
    return false;
  }
  return true;
}

bool QueryServiceState(DWORD* state) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
  if (!service) {
    CloseServiceHandle(manager);
    return false;
  }
  SERVICE_STATUS_PROCESS status{};
  DWORD needed = 0;
  const BOOL ok = QueryServiceStatusEx(
      service,
      SC_STATUS_PROCESS_INFO,
      reinterpret_cast<LPBYTE>(&status),
      sizeof(status),
      &needed);
  if (ok && state) *state = status.dwCurrentState;
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok != FALSE;
}

bool RestartService(std::wstring* error) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) {
    if (error) *error = L"无法连接 Windows 服务管理器：" + LastErrorMessage(GetLastError());
    return false;
  }
  SC_HANDLE service = OpenServiceW(
      manager,
      kServiceName,
      SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (!service) {
    if (error) *error = L"无法打开 DeskLink 服务：" + LastErrorMessage(GetLastError());
    CloseServiceHandle(manager);
    return false;
  }

  SERVICE_STATUS_PROCESS status{};
  DWORD needed = 0;
  if (QueryServiceStatusEx(
          service,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status),
          sizeof(status),
          &needed) &&
      status.dwCurrentState != SERVICE_STOPPED) {
    SERVICE_STATUS control{};
    ControlService(service, SERVICE_CONTROL_STOP, &control);
    for (int i = 0; i < 40; ++i) {
      Sleep(250);
      if (!QueryServiceStatusEx(
              service,
              SC_STATUS_PROCESS_INFO,
              reinterpret_cast<LPBYTE>(&status),
              sizeof(status),
              &needed) ||
          status.dwCurrentState == SERVICE_STOPPED) {
        break;
      }
    }
  }

  if (!StartServiceW(service, 0, nullptr)) {
    const DWORD start_error = GetLastError();
    if (start_error != ERROR_SERVICE_ALREADY_RUNNING) {
      if (error) *error = L"DeskLink 服务启动失败：" + LastErrorMessage(start_error);
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return false;
    }
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return true;
}

std::wstring ServiceStateText() {
  DWORD state = 0;
  if (!QueryServiceState(&state)) return L"未安装";
  switch (state) {
    case SERVICE_RUNNING: return L"运行中";
    case SERVICE_START_PENDING: return L"正在启动";
    case SERVICE_STOP_PENDING: return L"正在停止";
    case SERVICE_STOPPED: return L"已停止";
    case SERVICE_PAUSED: return L"已暂停";
    default: return L"状态未知";
  }
}

void UpdateStatus(HWND window, const std::wstring& detail = {}) {
  std::wstring text = L"服务状态：" + ServiceStateText();
  if (!detail.empty()) text += L"  ·  " + detail;
  SetControlText(window, kStatusText, text);
}

void ShowError(HWND window, const std::wstring& message) {
  MessageBoxW(window, message.c_str(), L"DeskLink 设置", MB_OK | MB_ICONERROR);
  UpdateStatus(window, L"操作失败");
}

bool HasProtectedAccessCode() {
  std::wstring value;
  std::wstring error;
  const auto status = desklink::LoadProtectedAccessCode(&value, &error);
  SecureClear(&value);
  return status == desklink::ProtectedCredentialStatus::Loaded;
}

bool HasProtectedDeviceCredential() {
  std::wstring value;
  std::wstring error;
  const auto status = desklink::LoadProtectedDeviceCredential(&value, &error);
  SecureClear(&value);
  return status == desklink::ProtectedCredentialStatus::Loaded;
}

void RunDiagnostics(HWND window) {
  EnableWindow(GetDlgItem(window, kDiagnoseButton), FALSE);
  SetControlText(window, kDiagnosticText, L"正在检查 Signal、Service、TURN 和 GPU 媒体管线…");
  UpdateWindow(window);

  int failures = 0;
  int warnings = 0;
  std::wstring report = L"DeskLink 连接诊断\r\n";
  report += L"----------------------------------------\r\n";
  auto pass = [&](const std::wstring& message) { report += L"[通过] " + message + L"\r\n"; };
  auto warn = [&](const std::wstring& message) {
    ++warnings;
    report += L"[警告] " + message + L"\r\n";
  };
  auto fail = [&](const std::wstring& message) {
    ++failures;
    report += L"[失败] " + message + L"\r\n";
  };

  const std::wstring signal_url = GetControlText(window, kSignalUrl);
  const std::wstring device_id = GetControlText(window, kDeviceId);
  const std::wstring stun_url = GetControlText(window, kStunUrl);
  const std::wstring turn_host = GetControlText(window, kTurnHost);
  const std::wstring turn_port = GetControlText(window, kTurnPort).empty()
      ? L"3478"
      : GetControlText(window, kTurnPort);
  const std::wstring signal_token_url = GetControlText(window, kSignalTokenUrl);
  const std::wstring turn_credentials_url = GetControlText(window, kTurnCredentialsUrl);

  DWORD service_state = 0;
  if (QueryServiceState(&service_state) && service_state == SERVICE_RUNNING) {
    pass(L"Windows Service 正在运行");
  } else if (service_state == SERVICE_STOPPED) {
    fail(L"Windows Service 已安装但未运行");
  } else {
    fail(L"Windows Service 未安装或状态不可用");
  }

  if (ValidDeviceId(device_id)) {
    pass(L"设备 ID 格式正确：" + device_id);
  } else {
    fail(L"设备 ID 格式不正确");
  }

  if (HasProtectedAccessCode()) {
    pass(L"无人值守访问码已使用 DPAPI 保存");
  } else {
    warn(L"没有检测到已保存的访问码；首次安装前需要设置");
  }

  const auto media_probe = MediaProbePath();
  if (GetFileAttributesW(media_probe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    warn(L"当前安装包没有媒体探针，无法自动检查显卡采集与 H.264 编码能力；请更新 DeskLink");
  } else {
    DWORD media_exit = 1;
    std::wstring media_error;
    if (RunProcessAndWait(media_probe, L"0", &media_exit, &media_error, 8000)) {
      if (media_exit == 0) {
        pass(L"本机媒体管线可用：D3D11 / DXGI 抓屏 / GPU NV12 / 硬件 H.264 已通过实测");
      } else {
        fail(MediaProbeFailureText(media_exit));
      }
    } else {
      fail(L"媒体管线探针无法完成：" + media_error + L" 建议更新显卡驱动后重试");
    }
  }

  if (StartsWith(signal_url, L"ws://") || StartsWith(signal_url, L"wss://")) {
    const std::wstring health_url = SignalHealthUrl(signal_url);
    std::wstring health_detail;
    if (!health_url.empty() && HttpHealthCheck(health_url, &health_detail)) {
      pass(L"Signal 服务可达：" + health_detail);
    } else {
      fail(health_detail.empty() ? L"Signal 服务健康检查失败" : health_detail);
    }
    if (StartsWith(signal_url, L"ws://") && signal_url.find(L"localhost") == std::wstring::npos &&
        signal_url.find(L"127.0.0.1") == std::wstring::npos) {
      warn(L"公网 Signal 使用 ws://，建议生产环境改为 wss://");
    }
  } else {
    fail(L"Signal 地址必须以 ws:// 或 wss:// 开头");
  }

  if (stun_url.empty()) {
    warn(L"未配置 STUN；跨公网直连成功率会降低");
  } else if (StartsWith(stun_url, L"stun:") || StartsWith(stun_url, L"stuns:")) {
    pass(L"STUN 地址格式已配置");
  } else {
    warn(L"STUN 地址格式异常，应以 stun: 或 stuns: 开头");
  }

  if (turn_host.empty()) {
    warn(L"未配置 TURN；复杂 NAT/企业网络下可能无法建立远程连接");
  } else {
    std::wstring turn_detail;
    if (TestTcpEndpoint(turn_host, turn_port, &turn_detail)) {
      pass(L"TURN TCP 探测成功：" + turn_detail);
    } else {
      warn(turn_detail + L"；UDP/TLS TURN 仍需由服务端和防火墙单独确认");
    }
  }

  if (!signal_token_url.empty()) {
    if (HasProtectedDeviceCredential()) {
      pass(L"Signal Token 模式已检测到加密设备凭证");
    } else {
      fail(L"启用了 Signal Token 接口，但没有检测到 dc2 设备凭证");
    }
  }
  if (!turn_credentials_url.empty()) {
    pass(L"已配置短期 TURN 凭证接口");
  } else if (!turn_host.empty()) {
    warn(L"TURN 已配置但没有短期凭证接口；生产环境建议启用动态 TURN 凭证");
  }

  report += L"----------------------------------------\r\n";
  if (failures == 0 && warnings == 0) {
    report += L"结论：配置完整，网络与本机媒体管线基础检查全部通过。\r\n";
  } else if (failures == 0) {
    report += L"结论：没有阻断项，但有 " + std::to_wstring(warnings) + L" 项建议优化。\r\n";
  } else {
    report += L"结论：发现 " + std::to_wstring(failures) + L" 个阻断项、" +
              std::to_wstring(warnings) + L" 个警告项。优先修复 [失败] 项。\r\n";
  }

  g_last_diagnostics = report;
  SetControlText(window, kDiagnosticText, report);
  EnableWindow(GetDlgItem(window, kCopyDiagnosticsButton), TRUE);
  EnableWindow(GetDlgItem(window, kDiagnoseButton), TRUE);
  UpdateStatus(window, failures == 0 ? L"诊断完成" : L"诊断发现问题");
}

void InstallOrUpdate(HWND window) {
  std::wstring signal_url = GetControlText(window, kSignalUrl);
  std::wstring device_id = GetControlText(window, kDeviceId);
  std::wstring access_code = GetControlText(window, kAccessCode);
  std::wstring stun_url = GetControlText(window, kStunUrl);
  std::wstring turn_host = GetControlText(window, kTurnHost);
  std::wstring turn_port = GetControlText(window, kTurnPort);
  std::wstring signal_token_url = GetControlText(window, kSignalTokenUrl);
  std::wstring turn_credentials_url = GetControlText(window, kTurnCredentialsUrl);
  std::wstring device_credential = GetControlText(window, kDeviceCredential);
  std::wstring fps_text = GetControlText(window, kFps);

  auto cleanup = [&]() {
    SecureClear(&access_code);
    SecureClear(&device_credential);
    SetControlText(window, kAccessCode, L"");
    SetControlText(window, kDeviceCredential, L"");
  };

  if (!(StartsWith(signal_url, L"ws://") || StartsWith(signal_url, L"wss://"))) {
    cleanup();
    ShowError(window, L"信令服务器地址必须以 ws:// 或 wss:// 开头。\n\n公网部署建议使用 wss://。");
    return;
  }
  if (!ValidDeviceId(device_id)) {
    cleanup();
    ShowError(window, L"设备 ID 只能包含字母、数字、点、短横线和下划线，长度 1–128。");
    return;
  }
  int parsed_port = 0;
  try {
    parsed_port = std::stoi(turn_port.empty() ? L"3478" : turn_port);
  } catch (...) {
    parsed_port = 0;
  }
  if (parsed_port < 1 || parsed_port > 65535) {
    cleanup();
    ShowError(window, L"TURN 端口必须是 1–65535。");
    return;
  }
  int parsed_fps = 0;
try {
  parsed_fps = std::stoi(fps_text.empty() ? L"60" : fps_text);
} catch (...) {
  parsed_fps = 0;
}
if (parsed_fps < 15 || parsed_fps > 144) {
  cleanup();
  ShowError(window, L"目标帧率必须是 15–144 FPS。普通办公建议 60；高刷设备可使用 90 / 120 / 144。");
  return;
}
if (!access_code.empty() && (access_code.size() < 8 || access_code.size() > 256)) {
    cleanup();
    ShowError(window, L"访问码长度必须为 8–256 个字符。建议使用随机高强度访问码。");
    return;
  }
  if (access_code.empty() && !HasProtectedAccessCode()) {
    cleanup();
    ShowError(window, L"首次安装必须设置访问码。后续更新时可以留空，以保留已加密保存的访问码。");
    return;
  }
  if (!signal_token_url.empty() && device_credential.empty() && !HasProtectedDeviceCredential()) {
    cleanup();
    ShowError(
        window,
        L"你启用了信令令牌接口，但当前没有设备凭证。\n\n请输入服务器生成的 dc2 设备凭证，或清空信令令牌接口使用开发模式。");
    return;
  }

  EnableWindow(GetDlgItem(window, kInstallButton), FALSE);
  UpdateStatus(window, L"正在安装/更新…");

  std::wstring error;
  std::filesystem::path service_path;
  if (!PrepareInstallDirectory(&service_path, &error)) {
    EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
    cleanup();
    ShowError(window, error);
    return;
  }

  DWORD install_exit = 1;
  if (!RunProcessAndWait(service_path, L"--install", &install_exit, &error) || install_exit != 0) {
    EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
    cleanup();
    if (error.empty()) error = L"DeskLink 服务安装失败，退出代码 " + std::to_wstring(install_exit) + L"。";
    ShowError(window, error);
    return;
  }

  std::vector<std::pair<std::wstring, std::wstring>> settings = {
      {L"DESKLINK_SIGNAL_URL", signal_url},
      {L"DESKLINK_DEVICE_ID", device_id},
      {L"DESKLINK_STUN_URL", stun_url},
      {L"DESKLINK_TURN_HOST", turn_host},
      {L"DESKLINK_TURN_PORT", std::to_wstring(parsed_port)},
      {L"DESKLINK_TURN_TLS_PORT", L"5349"},
      {L"DESKLINK_SIGNAL_TOKEN_URL", signal_token_url},
      {L"DESKLINK_TURN_CREDENTIALS_URL", turn_credentials_url},
      {L"DESKLINK_OUTPUT_INDEX", L"0"},
      {L"DESKLINK_FPS", std::to_wstring(parsed_fps)},
  };
  if (!signal_token_url.empty()) settings.emplace_back(L"DESKLINK_SIGNAL_TOKEN_REQUIRED", L"1");
  if (!turn_credentials_url.empty()) settings.emplace_back(L"DESKLINK_TURN_RUNTIME_REQUIRED", L"1");

  if (!WriteServiceEnvironment(settings, &error)) {
    EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
    cleanup();
    ShowError(window, error);
    return;
  }

  if (!access_code.empty()) {
    if (!desklink::StoreProtectedAccessCode(access_code, &error)) {
      EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
      cleanup();
      ShowError(window, L"访问码加密保存失败：" + error);
      return;
    }
  }
  if (!device_credential.empty()) {
    if (!desklink::StoreProtectedDeviceCredential(device_credential, &error)) {
      EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
      cleanup();
      ShowError(window, L"设备凭证加密保存失败：" + error);
      return;
    }
  }
  if (!RestartService(&error)) {
    EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
    cleanup();
    ShowError(window, error);
    return;
  }

  cleanup();
  EnableWindow(GetDlgItem(window, kInstallButton), TRUE);
  UpdateStatus(window, L"配置已应用");
  RunDiagnostics(window);
  MessageBoxW(
      window,
      L"DeskLink Windows 主机已安装并启动。\n\n"
      L"设备 ID 已配置；访问码已使用 Windows DPAPI 加密保存。\n"
      L"下方连接诊断会检查 Signal / TURN / Service，并实测本机 GPU 抓屏与 H.264 编码能力。",
      L"DeskLink 设置完成",
      MB_OK | MB_ICONINFORMATION);
}

HWND AddLabel(HWND window, const wchar_t* text, int x, int y, int width) {
  HWND label = CreateWindowExW(
      0,
      L"STATIC",
      text,
      WS_CHILD | WS_VISIBLE,
      x,
      y,
      width,
      22,
      window,
      nullptr,
      nullptr,
      nullptr);
  SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return label;
}

HWND AddEdit(
    HWND window,
    int id,
    int x,
    int y,
    int width,
    const wchar_t* cue,
    bool password = false) {
  DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
  if (password) style |= ES_PASSWORD;
  HWND edit = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      L"",
      style,
      x,
      y,
      width,
      30,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      nullptr,
      nullptr);
  SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
  return edit;
}

HWND AddButton(HWND window, int id, const wchar_t* text, int x, int y, int width, bool primary = false) {
  HWND button = CreateWindowExW(
      0,
      L"BUTTON",
      text,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
      x,
      y,
      width,
      38,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      nullptr,
      nullptr);
  SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return button;
}

void Prefill(HWND window) {
  const auto environment = ReadServiceEnvironment();
  auto from_env = [&](const wchar_t* key, const wchar_t* fallback) {
    const auto it = environment.find(key);
    return it == environment.end() ? std::wstring(fallback) : it->second;
  };

  SetControlText(window, kSignalUrl, from_env(L"DESKLINK_SIGNAL_URL", L"ws://localhost:8080/ws"));
  SetControlText(window, kDeviceId, from_env(L"DESKLINK_DEVICE_ID", DefaultDeviceId().c_str()));
  SetControlText(window, kFps, from_env(L"DESKLINK_FPS", L"60"));
  SetControlText(window, kStunUrl, from_env(L"DESKLINK_STUN_URL", L""));
  SetControlText(window, kTurnHost, from_env(L"DESKLINK_TURN_HOST", L""));
  SetControlText(window, kTurnPort, from_env(L"DESKLINK_TURN_PORT", L"3478"));
  SetControlText(window, kSignalTokenUrl, from_env(L"DESKLINK_SIGNAL_TOKEN_URL", L""));
  SetControlText(window, kTurnCredentialsUrl, from_env(L"DESKLINK_TURN_CREDENTIALS_URL", L""));
  UpdateStatus(window);
}

void CreateControls(HWND window) {
  g_font = CreateFontW(
      -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_title_font = CreateFontW(
      -26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  HWND title = CreateWindowExW(
      0, L"STATIC", L"DeskLink Windows 主机", WS_CHILD | WS_VISIBLE,
      28, 22, 650, 34, window, nullptr, nullptr, nullptr);
  SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), TRUE);

  HWND subtitle = CreateWindowExW(
      0,
      L"STATIC",
      L"配置被控端并一键检查 Signal、TURN、Windows Service 与本机 GPU 媒体管线。配置更新后无需重启 Windows。",
      WS_CHILD | WS_VISIBLE,
      30,
      60,
      660,
      38,
      window,
      nullptr,
      nullptr,
      nullptr);
  SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  int y = 112;
  constexpr int label_x = 30;
  constexpr int edit_x = 190;
  constexpr int edit_width = 490;
  constexpr int row = 46;

  AddLabel(window, L"信令服务器 *", label_x, y + 5, 150);
  AddEdit(window, kSignalUrl, edit_x, y, edit_width, L"wss://control.example.com/ws");
  y += row;
  AddLabel(window, L"设备 ID *", label_x, y + 5, 150);
  AddEdit(window, kDeviceId, edit_x, y, edit_width, L"例如 office-pc-01");
  y += row;
  AddLabel(window, L"访问码 *", label_x, y + 5, 150);
  AddEdit(window, kAccessCode, edit_x, y, edit_width, L"首次必填；留空保留现有访问码", true);
  y += row;
  AddLabel(window, L"STUN 地址", label_x, y + 5, 150);
  AddEdit(window, kStunUrl, edit_x, y, edit_width, L"例如 stun:turn.example.com:3478");
  y += row;
  AddLabel(window, L"TURN 主机", label_x, y + 5, 150);
  AddEdit(window, kTurnHost, edit_x, y, 370, L"例如 turn.example.com");
  AddEdit(window, kTurnPort, 570, y, 110, L"3478");
  y += row;
  AddLabel(window, L"信令令牌接口", label_x, y + 5, 150);
  AddEdit(window, kSignalTokenUrl, edit_x, y, edit_width, L"可选：https://.../api/v1/signal-token");
  y += row;
  AddLabel(window, L"TURN 凭证接口", label_x, y + 5, 150);
  AddEdit(window, kTurnCredentialsUrl, edit_x, y, edit_width, L"可选：https://.../api/v1/turn-credentials");
  y += row;
  AddLabel(window, L"设备凭证", label_x, y + 5, 150);
  AddEdit(window, kDeviceCredential, edit_x, y, edit_width, L"启用信令鉴权时填写 dc2...；留空保留", true);
  y += row;
  AddLabel(window, L"目标帧率 FPS", label_x, y + 5, 150);
  AddEdit(window, kFps, edit_x, y, edit_width, L"60（可选 90 / 120 / 144）");

  HWND hint = CreateWindowExW(
      0,
      L"STATIC",
      L"建议：安装后先点击“连接诊断”。它会实测抓屏/硬件 H.264，并检查 WSS、STUN、TURN 与服务状态。",
      WS_CHILD | WS_VISIBLE,
      30,
      y + 48,
      650,
      42,
      window,
      nullptr,
      nullptr,
      nullptr);
  SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  const int action_y = y + 100;
  AddButton(window, kInstallButton, L"安装 / 更新并启动", 30, action_y, 190, true);
  AddButton(window, kDiagnoseButton, L"连接诊断", 230, action_y, 110);
  AddButton(window, kCopyDeviceButton, L"复制设备 ID", 350, action_y, 120);
  AddButton(window, kRefreshButton, L"刷新状态", 480, action_y, 100);
  HWND copy_diagnostics = AddButton(window, kCopyDiagnosticsButton, L"复制诊断", 590, action_y, 90);
  EnableWindow(copy_diagnostics, FALSE);

  HWND status = CreateWindowExW(
      0,
      L"STATIC",
      L"服务状态：检查中…",
      WS_CHILD | WS_VISIBLE,
      30,
      y + 153,
      650,
      26,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)),
      nullptr,
      nullptr);
  SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  HWND diagnostic = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      L"点击“连接诊断”检查当前配置和本机媒体能力。不会上传访问码或设备凭证。",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      30,
      y + 183,
      650,
      150,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiagnosticText)),
      nullptr,
      nullptr);
  SendMessageW(diagnostic, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  Prefill(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  switch (message) {
    case WM_CREATE:
      CreateControls(window);
      return 0;
    case WM_COMMAND:
      switch (LOWORD(w_param)) {
        case kInstallButton:
          if (HIWORD(w_param) == BN_CLICKED) InstallOrUpdate(window);
          return 0;
        case kDiagnoseButton:
          if (HIWORD(w_param) == BN_CLICKED) RunDiagnostics(window);
          return 0;
        case kCopyDeviceButton:
          if (HIWORD(w_param) == BN_CLICKED) {
            const std::wstring device_id = GetControlText(window, kDeviceId);
            if (CopyTextToClipboard(window, device_id)) {
              UpdateStatus(window, L"设备 ID 已复制");
            } else {
              ShowError(window, L"复制设备 ID 失败。");
            }
          }
          return 0;
        case kCopyDiagnosticsButton:
          if (HIWORD(w_param) == BN_CLICKED && !g_last_diagnostics.empty()) {
            if (CopyTextToClipboard(window, g_last_diagnostics)) {
              UpdateStatus(window, L"诊断报告已复制");
            } else {
              ShowError(window, L"复制诊断报告失败。");
            }
          }
          return 0;
        case kRefreshButton:
          if (HIWORD(w_param) == BN_CLICKED) UpdateStatus(window);
          return 0;
        default:
          break;
      }
      break;
    case WM_DESTROY:
      if (g_title_font) DeleteObject(g_title_font);
      if (g_font) DeleteObject(g_font);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  INITCOMMONCONTROLSEX common_controls{};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&common_controls);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) return 1;

  HWND window = CreateWindowExW(
      0,
      kWindowClass,
      L"DeskLink 设置与诊断",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      730,
      900,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (!window) return 1;

  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}