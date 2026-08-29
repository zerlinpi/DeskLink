#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
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

HFONT g_font = nullptr;
HFONT g_title_font = nullptr;

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

std::wstring GetControlText(HWND parent, int id) {
  HWND control = GetDlgItem(parent, id);
  if (!control) return {};
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};
  std::wstring text(static_cast<size_t>(length), L'\0');
  GetWindowTextW(control, text.data(), length + 1);
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
    std::wstring* error) {
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
    if (error) *error = L"启动服务安装程序失败：" + LastErrorMessage(GetLastError());
    return false;
  }

  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    if (error) *error = L"服务安装超时。";
    return false;
  }

  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  if (exit_code) *exit_code = code;
  return true;
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
    if (error) *error = L"无法写入 DeskLink 服务配置：" + LastErrorMessage(open);
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
    if (error) *error = L"保存 DeskLink 服务配置失败：" + LastErrorMessage(set);
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
    ShowError(window, L"设备 ID 只能包含字母、数字、点、短横线和下划线，长度 1–128。 ");
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
    ShowError(window, L"TURN 端口必须是 1–65535。 ");
    return;
  }
  if (!access_code.empty() && (access_code.size() < 8 || access_code.size() > 256)) {
    cleanup();
    ShowError(window, L"访问码长度必须为 8–256 个字符。建议使用随机高强度访问码。 ");
    return;
  }
  if (access_code.empty() && !HasProtectedAccessCode()) {
    cleanup();
    ShowError(window, L"首次安装必须设置访问码。后续更新时可以留空，以保留已加密保存的访问码。 ");
    return;
  }
  if (!signal_token_url.empty() && device_credential.empty() && !HasProtectedDeviceCredential()) {
    cleanup();
    ShowError(
        window,
        L"你启用了信令令牌接口，但当前没有设备凭证。\n\n请输入服务器生成的 dc2 设备凭证，或清空信令令牌接口使用开发模式。 ");
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
  MessageBoxW(
      window,
      L"DeskLink Windows 主机已安装并启动。\n\n"
      L"设备 ID 已配置；访问码已使用 Windows DPAPI 加密保存。\n"
      L"现在可以在 Web 控制端输入设备 ID 和访问码发起连接。",
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

void Prefill(HWND window) {
  const auto environment = ReadServiceEnvironment();
  auto from_env = [&](const wchar_t* key, const wchar_t* fallback) {
    const auto it = environment.find(key);
    return it == environment.end() ? std::wstring(fallback) : it->second;
  };

  SetControlText(window, kSignalUrl, from_env(L"DESKLINK_SIGNAL_URL", L"ws://localhost:8080/ws"));
  SetControlText(window, kDeviceId, from_env(L"DESKLINK_DEVICE_ID", DefaultDeviceId().c_str()));
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
      28, 22, 620, 34, window, nullptr, nullptr, nullptr);
  SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), TRUE);

  HWND subtitle = CreateWindowExW(
      0,
      L"STATIC",
      L"配置被控端连接参数、无人值守访问码并安装后台服务。配置更新后无需重启 Windows。",
      WS_CHILD | WS_VISIBLE,
      30,
      60,
      630,
      38,
      window,
      nullptr,
      nullptr,
      nullptr);
  SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  int y = 112;
  constexpr int label_x = 30;
  constexpr int edit_x = 190;
  constexpr int edit_width = 455;
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
  AddEdit(window, kTurnHost, edit_x, y, 340, L"例如 turn.example.com");
  AddEdit(window, kTurnPort, 540, y, 105, L"3478");
  y += row;
  AddLabel(window, L"信令令牌接口", label_x, y + 5, 150);
  AddEdit(window, kSignalTokenUrl, edit_x, y, edit_width, L"可选：https://.../api/v1/signal-token");
  y += row;
  AddLabel(window, L"TURN 凭证接口", label_x, y + 5, 150);
  AddEdit(window, kTurnCredentialsUrl, edit_x, y, edit_width, L"可选：https://.../api/v1/turn-credentials");
  y += row;
  AddLabel(window, L"设备凭证", label_x, y + 5, 150);
  AddEdit(window, kDeviceCredential, edit_x, y, edit_width, L"启用信令鉴权时填写 dc2...；留空保留", true);

  HWND hint = CreateWindowExW(
      0,
      L"STATIC",
      L"提示：公网使用需先部署 DeskLink Signal/TURN。正式环境建议使用 WSS、短期 TURN 凭证和 dc2 设备凭证。",
      WS_CHILD | WS_VISIBLE,
      30,
      y + 48,
      615,
      42,
      window,
      nullptr,
      nullptr,
      nullptr);
  SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  HWND install = CreateWindowExW(
      0,
      L"BUTTON",
      L"安装 / 更新并启动",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      30,
      y + 100,
      210,
      38,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallButton)),
      nullptr,
      nullptr);
  SendMessageW(install, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  HWND refresh = CreateWindowExW(
      0,
      L"BUTTON",
      L"刷新服务状态",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      250,
      y + 100,
      150,
      38,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)),
      nullptr,
      nullptr);
  SendMessageW(refresh, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

  HWND status = CreateWindowExW(
      0,
      L"STATIC",
      L"服务状态：检查中…",
      WS_CHILD | WS_VISIBLE,
      30,
      y + 153,
      615,
      26,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)),
      nullptr,
      nullptr);
  SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

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
      L"DeskLink 设置",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      700,
      690,
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
