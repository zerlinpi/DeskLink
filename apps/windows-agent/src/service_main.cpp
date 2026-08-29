#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kServiceName[] = L"DeskLink";
constexpr wchar_t kServiceDisplayName[] = L"DeskLink Remote Desktop";
constexpr wchar_t kServiceDescription[] =
    L"Keeps the DeskLink remote desktop agent running in the active Windows user session.";
constexpr DWORD kNoSession = 0xFFFFFFFF;

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;
HANDLE g_session_event = nullptr;
HANDLE g_agent_process = nullptr;
HANDLE g_agent_job = nullptr;
DWORD g_agent_session = kNoSession;
DWORD g_checkpoint = 1;

void LogEvent(WORD type, const std::wstring& message) {
  OutputDebugStringW((L"DeskLink Service: " + message + L"\n").c_str());
  HANDLE source = RegisterEventSourceW(nullptr, kServiceName);
  if (!source) return;
  LPCWSTR strings[] = {message.c_str()};
  ReportEventW(source, type, 0, 0, nullptr, 1, 0, strings, nullptr);
  DeregisterEventSource(source);
}

void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
  if (!g_status_handle) return;

  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32_exit_code;
  g_status.dwWaitHint = wait_hint;
  g_status.dwControlsAccepted = 0;
  if (state == SERVICE_RUNNING) {
    g_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  }

  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
    g_status.dwCheckPoint = g_checkpoint++;
  } else {
    g_status.dwCheckPoint = 0;
  }
  SetServiceStatus(g_status_handle, &g_status);
}

bool EnablePrivilege(const wchar_t* privilege_name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(
          GetCurrentProcess(),
          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
          &token)) {
    return false;
  }

  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) {
    CloseHandle(token);
    return false;
  }

  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Luid = luid;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(
      token,
      FALSE,
      &privileges,
      sizeof(privileges),
      nullptr,
      nullptr);
  const DWORD error = GetLastError();
  CloseHandle(token);
  return adjusted && error == ERROR_SUCCESS;
}

std::filesystem::path CurrentExecutablePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(
      nullptr,
      buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path AgentExecutablePath() {
  const std::filesystem::path service_path = CurrentExecutablePath();
  if (service_path.empty()) return {};
  return service_path.parent_path() / L"desklink-agent.exe";
}

std::wstring OutputIndexArgument() {
  wchar_t buffer[16]{};
  const DWORD length = GetEnvironmentVariableW(
      L"DESKLINK_OUTPUT_INDEX",
      buffer,
      static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return {};

  std::wstring value(buffer, length);
  if (value.empty()) return {};
  for (wchar_t ch : value) {
    if (!std::iswdigit(ch)) return {};
  }
  return value;
}

bool AgentAlive() {
  return g_agent_process && WaitForSingleObject(g_agent_process, 0) == WAIT_TIMEOUT;
}

void StopAgent() {
  if (g_agent_job) {
    TerminateJobObject(g_agent_job, 0);
    CloseHandle(g_agent_job);
    g_agent_job = nullptr;
  } else if (g_agent_process && AgentAlive()) {
    TerminateProcess(g_agent_process, 0);
  }

  if (g_agent_process) {
    WaitForSingleObject(g_agent_process, 3000);
    CloseHandle(g_agent_process);
    g_agent_process = nullptr;
  }
  g_agent_session = kNoSession;
}

bool LaunchAgent(DWORD session_id) {
  if (session_id == kNoSession) return false;

  const std::filesystem::path agent_path = AgentExecutablePath();
  if (agent_path.empty() || GetFileAttributesW(agent_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogEvent(EVENTLOG_ERROR_TYPE, L"desklink-agent.exe was not found beside desklink-service.exe");
    return false;
  }

  HANDLE user_token = nullptr;
  if (!WTSQueryUserToken(session_id, &user_token)) {
    LogEvent(
        EVENTLOG_WARNING_TYPE,
        L"WTSQueryUserToken failed for session " + std::to_wstring(session_id) +
            L" (error " + std::to_wstring(GetLastError()) + L")");
    return false;
  }

  LPVOID environment = nullptr;
  const BOOL has_environment = CreateEnvironmentBlock(&environment, user_token, FALSE);

  std::wstring command = L"\"" + agent_path.wstring() + L"\"";
  const std::wstring output_index = OutputIndexArgument();
  if (!output_index.empty()) command += L" " + output_index;
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  std::wstring desktop = L"winsta0\\default";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.lpDesktop = desktop.data();

  PROCESS_INFORMATION process{};
  DWORD creation_flags = CREATE_NO_WINDOW;
  if (has_environment) creation_flags |= CREATE_UNICODE_ENVIRONMENT;

  const std::wstring working_directory = agent_path.parent_path().wstring();
  const BOOL created = CreateProcessAsUserW(
      user_token,
      agent_path.c_str(),
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      creation_flags,
      has_environment ? environment : nullptr,
      working_directory.c_str(),
      &startup,
      &process);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

  if (has_environment) DestroyEnvironmentBlock(environment);
  CloseHandle(user_token);

  if (!created) {
    LogEvent(
        EVENTLOG_ERROR_TYPE,
        L"CreateProcessAsUser failed for session " + std::to_wstring(session_id) +
            L" (error " + std::to_wstring(create_error) + L")");
    return false;
  }

  CloseHandle(process.hThread);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      CloseHandle(job);
      job = nullptr;
    }
  }

  g_agent_process = process.hProcess;
  g_agent_job = job;
  g_agent_session = session_id;
  LogEvent(
      EVENTLOG_INFORMATION_TYPE,
      L"Started desklink-agent.exe in session " + std::to_wstring(session_id));
  return true;
}

DWORD WINAPI ServiceControlHandler(
    DWORD control,
    DWORD event_type,
    LPVOID,
    LPVOID) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
      if (g_stop_event) SetEvent(g_stop_event);
      return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
      if (g_session_event) SetEvent(g_session_event);
      LogEvent(
          EVENTLOG_INFORMATION_TYPE,
          L"Windows session change received (event " + std::to_wstring(event_type) + L")");
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      ReportStatus(g_status.dwCurrentState);
      return NO_ERROR;
    default:
      return NO_ERROR;
  }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(
      kServiceName,
      ServiceControlHandler,
      nullptr);
  if (!g_status_handle) return;

  ReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_session_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stop_event || !g_session_event) {
    ReportStatus(SERVICE_STOPPED, GetLastError());
    return;
  }

  EnablePrivilege(SE_TCB_NAME);
  EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
  EnablePrivilege(SE_INCREASE_QUOTA_NAME);

  ReportStatus(SERVICE_RUNNING);
  LogEvent(EVENTLOG_INFORMATION_TYPE, L"DeskLink service started");

  using clock = std::chrono::steady_clock;
  auto next_launch_attempt = clock::now();
  auto agent_started_at = clock::time_point{};
  uint32_t consecutive_failures = 0;

  auto schedule_retry = [&](const wchar_t* reason) {
    consecutive_failures = std::min<uint32_t>(consecutive_failures + 1, 6);
    const uint32_t delay_seconds = std::min<uint32_t>(60, 1u << consecutive_failures);
    next_launch_attempt = clock::now() + std::chrono::seconds(delay_seconds);
    LogEvent(
        EVENTLOG_WARNING_TYPE,
        std::wstring(reason) + L"; retrying Agent in " +
            std::to_wstring(delay_seconds) + L" seconds");
  };

  while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
    const auto now = clock::now();
    const DWORD active_session = WTSGetActiveConsoleSessionId();
    const bool session_changed = active_session != g_agent_session;
    const bool agent_dead = g_agent_process && !AgentAlive();

    if (session_changed) {
      StopAgent();
      consecutive_failures = 0;
      agent_started_at = clock::time_point{};
      next_launch_attempt = now;
    } else if (agent_dead) {
      const bool was_stable = agent_started_at != clock::time_point{} &&
                              now - agent_started_at >= std::chrono::seconds(60);
      StopAgent();
      if (was_stable) consecutive_failures = 0;
      schedule_retry(L"desklink-agent.exe exited unexpectedly");
      agent_started_at = clock::time_point{};
    }

    if (!g_agent_process && active_session != kNoSession &&
        clock::now() >= next_launch_attempt) {
      if (LaunchAgent(active_session)) {
        agent_started_at = clock::now();
      } else {
        schedule_retry(L"Unable to launch desklink-agent.exe");
      }
    }

    HANDLE wait_handles[] = {g_stop_event, g_session_event};
    const DWORD wait = WaitForMultipleObjects(2, wait_handles, FALSE, 2000);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_OBJECT_0 + 1) ResetEvent(g_session_event);
  }

  StopAgent();
  if (g_session_event) {
    CloseHandle(g_session_event);
    g_session_event = nullptr;
  }
  if (g_stop_event) {
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
  }

  LogEvent(EVENTLOG_INFORMATION_TYPE, L"DeskLink service stopped");
  ReportStatus(SERVICE_STOPPED);
}

void ConfigureFailureRecovery(SC_HANDLE service) {
  SC_ACTION actions[3] = {
      {SC_ACTION_RESTART, 5000},
      {SC_ACTION_RESTART, 10000},
      {SC_ACTION_RESTART, 30000},
  };
  SERVICE_FAILURE_ACTIONSW failure{};
  failure.dwResetPeriod = 24 * 60 * 60;
  failure.cActions = static_cast<DWORD>(std::size(actions));
  failure.lpsaActions = actions;
  ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

  SERVICE_DESCRIPTIONW description{};
  description.lpDescription = const_cast<LPWSTR>(kServiceDescription);
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
}

int InstallService() {
  const std::filesystem::path executable = CurrentExecutablePath();
  if (executable.empty()) {
    std::wcerr << L"Unable to resolve desklink-service.exe path\n";
    return 1;
  }

  const std::wstring image_path = L"\"" + executable.wstring() + L"\" --service";
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (!manager) {
    std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
    return 1;
  }

  SC_HANDLE service = CreateServiceW(
      manager,
      kServiceName,
      kServiceDisplayName,
      SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS,
      SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL,
      image_path.c_str(),
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr);

  if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
    service = OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
    if (service && !ChangeServiceConfigW(
                       service,
                       SERVICE_NO_CHANGE,
                       SERVICE_AUTO_START,
                       SERVICE_NO_CHANGE,
                       image_path.c_str(),
                       nullptr,
                       nullptr,
                       nullptr,
                       nullptr,
                       nullptr,
                       kServiceDisplayName)) {
      std::wcerr << L"ChangeServiceConfig failed: " << GetLastError() << L"\n";
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return 1;
    }
  }

  if (!service) {
    std::wcerr << L"CreateService failed: " << GetLastError() << L"\n";
    CloseServiceHandle(manager);
    return 1;
  }

  ConfigureFailureRecovery(service);
  if (!StartServiceW(service, 0, nullptr)) {
    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_ALREADY_RUNNING) {
      std::wcerr << L"Service installed but could not start: " << error << L"\n";
    }
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  std::wcout << L"DeskLink service installed.\n";
  return 0;
}

int UninstallService() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) {
    std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
    return 1;
  }

  SC_HANDLE service = OpenServiceW(
      manager,
      kServiceName,
      SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
  if (!service) {
    const DWORD error = GetLastError();
    CloseServiceHandle(manager);
    if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
      std::wcout << L"DeskLink service is not installed.\n";
      return 0;
    }
    std::wcerr << L"OpenService failed: " << error << L"\n";
    return 1;
  }

  SERVICE_STATUS status{};
  ControlService(service, SERVICE_CONTROL_STOP, &status);
  for (int attempt = 0; attempt < 20; ++attempt) {
    SERVICE_STATUS_PROCESS process_status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&process_status),
            sizeof(process_status),
            &needed) ||
        process_status.dwCurrentState == SERVICE_STOPPED) {
      break;
    }
    Sleep(500);
  }

  const BOOL deleted = DeleteService(service);
  const DWORD delete_error = deleted ? ERROR_SUCCESS : GetLastError();
  CloseServiceHandle(service);
  CloseServiceHandle(manager);

  if (!deleted && delete_error != ERROR_SERVICE_MARKED_FOR_DELETE) {
    std::wcerr << L"DeleteService failed: " << delete_error << L"\n";
    return 1;
  }

  std::wcout << L"DeskLink service uninstalled.\n";
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2) {
    const std::wstring command = argv[1];
    if (command == L"--install") return InstallService();
    if (command == L"--uninstall") return UninstallService();
    if (command != L"--service") {
      std::wcerr << L"Usage: desklink-service.exe --install | --uninstall | --service\n";
      return 2;
    }
  }

  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(kServiceName), ServiceMain},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      std::wcerr << L"Run as administrator with --install to install the service.\n";
    } else {
      std::wcerr << L"StartServiceCtrlDispatcher failed: " << error << L"\n";
    }
    return 1;
  }
  return 0;
}
