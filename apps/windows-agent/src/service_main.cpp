#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "protected_credential_store.h"
#include "service_auth_broker.h"

namespace {

constexpr wchar_t kServiceName[] = L"DeskLink";
constexpr wchar_t kServiceDisplayName[] = L"DeskLink Remote Desktop";
constexpr wchar_t kServiceDescription[] =
    L"Keeps the DeskLink remote desktop agent running in the active Windows user session.";
constexpr DWORD kNoSession = 0xFFFFFFFF;
constexpr wchar_t kDeviceCredentialPrefix[] = L"DESKLINK_DEVICE_CREDENTIAL=";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;
HANDLE g_session_event = nullptr;
HANDLE g_agent_process = nullptr;
HANDLE g_agent_job = nullptr;
HANDLE g_agent_stop_event = nullptr;
std::wstring g_agent_stop_event_name;
DWORD g_agent_session = kNoSession;
DWORD g_checkpoint = 1;
uint64_t g_stop_event_counter = 0;
uint64_t g_auth_pipe_counter = 0;

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

std::wstring EnvironmentValue(const wchar_t* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0 || required > 32768) return {};
  std::vector<wchar_t> buffer(required);
  const DWORD length = GetEnvironmentVariableW(
      name,
      buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::wstring(buffer.data(), length);
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) return {};
  std::string result(static_cast<size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          required,
          nullptr,
          nullptr) != required) {
    return {};
  }
  return result;
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

void SecureWipe(std::wstring* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
  value->clear();
}

void SecureWipe(std::string* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size());
  value->clear();
}

void SecureWipe(std::vector<wchar_t>* value) {
  if (!value || value->empty()) return;
  SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
  value->clear();
}

std::vector<wchar_t> BuildEnvironmentWithoutDeviceCredential(LPVOID base_environment) {
  std::vector<std::wstring> entries;
  constexpr size_t prefix_length =
      (sizeof(kDeviceCredentialPrefix) / sizeof(kDeviceCredentialPrefix[0])) - 1;

  const auto* cursor = static_cast<const wchar_t*>(base_environment);
  while (cursor && *cursor != L'\0') {
    const size_t length = std::wcslen(cursor);
    const bool is_device_credential =
        length >= prefix_length &&
        _wcsnicmp(cursor, kDeviceCredentialPrefix, prefix_length) == 0;
    if (!is_device_credential) {
      entries.emplace_back(cursor, length);
    }
    cursor += length + 1;
  }

  std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  });

  std::vector<wchar_t> result;
  for (const auto& entry : entries) {
    result.insert(result.end(), entry.begin(), entry.end());
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  return result;
}

bool ReadCredentialFromConsole(std::wstring* credential) {
  if (!credential) return false;
  credential->clear();

  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD original_mode = 0;
  const bool has_console_mode =
      input != INVALID_HANDLE_VALUE && input != nullptr && GetConsoleMode(input, &original_mode);
  if (has_console_mode) {
    SetConsoleMode(input, original_mode & ~ENABLE_ECHO_INPUT);
  }

  std::wcout << L"Enter DeskLink dc1 device credential: " << std::flush;
  std::getline(std::wcin, *credential);

  if (has_console_mode) {
    SetConsoleMode(input, original_mode);
  }
  std::wcout << L"\n";
  return !credential->empty();
}

int StoreDeviceCredentialCommand() {
  std::wstring credential;
  if (!ReadCredentialFromConsole(&credential)) {
    std::wcerr << L"No credential was entered.\n";
    return 1;
  }

  std::wstring error;
  const bool stored = desklink::StoreProtectedDeviceCredential(credential, &error);
  SecureWipe(&credential);
  if (!stored) {
    std::wcerr << L"Unable to store device credential: " << error << L"\n";
    return 1;
  }

  std::wcout << L"Device credential stored with machine-scope DPAPI protection.\n"
             << L"Restart the DeskLink service to launch a new Agent with it.\n";
  return 0;
}

int ClearDeviceCredentialCommand() {
  std::wstring error;
  if (!desklink::DeleteProtectedDeviceCredential(&error)) {
    std::wcerr << L"Unable to clear protected device credential: " << error << L"\n";
    return 1;
  }
  std::wcout << L"Protected device credential removed.\n";
  return 0;
}

void CloseAgentStopEvent() {
  if (g_agent_stop_event) {
    CloseHandle(g_agent_stop_event);
    g_agent_stop_event = nullptr;
  }
  g_agent_stop_event_name.clear();
}

bool CreateAgentStopEvent(DWORD session_id) {
  CloseAgentStopEvent();

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GX;;;AU)",
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
    LogEvent(EVENTLOG_ERROR_TYPE, L"Unable to create Agent shutdown event security descriptor");
    return false;
  }

  const uint64_t nonce = ++g_stop_event_counter;
  g_agent_stop_event_name =
      L"Global\\DeskLink.AgentStop." + std::to_wstring(session_id) + L"." +
      std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." +
      std::to_wstring(nonce);

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  SetLastError(ERROR_SUCCESS);
  g_agent_stop_event = CreateEventW(
      &attributes,
      TRUE,
      FALSE,
      g_agent_stop_event_name.c_str());
  const DWORD create_error = GetLastError();
  LocalFree(descriptor);

  if (!g_agent_stop_event || create_error == ERROR_ALREADY_EXISTS) {
    if (g_agent_stop_event) CloseHandle(g_agent_stop_event);
    g_agent_stop_event = nullptr;
    g_agent_stop_event_name.clear();
    LogEvent(EVENTLOG_ERROR_TYPE, L"Unable to create unique Agent shutdown event");
    return false;
  }
  return true;
}

std::wstring NewServiceAuthPipeName(DWORD session_id) {
  const uint64_t nonce = ++g_auth_pipe_counter;
  return L"\\\\.\\pipe\\DeskLink.Auth." + std::to_wstring(session_id) + L"." +
         std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." +
         std::to_wstring(nonce);
}

bool AgentAlive() {
  return g_agent_process && WaitForSingleObject(g_agent_process, 0) == WAIT_TIMEOUT;
}

void StopAgent() {
  bool exited_gracefully = false;
  if (g_agent_process && AgentAlive() && g_agent_stop_event) {
    if (SetEvent(g_agent_stop_event)) {
      exited_gracefully = WaitForSingleObject(g_agent_process, 5000) == WAIT_OBJECT_0;
    }
  }

  if (g_agent_process && AgentAlive()) {
    if (!exited_gracefully) {
      LogEvent(
          EVENTLOG_WARNING_TYPE,
          L"Agent did not exit within the graceful shutdown window; forcing termination");
    }
    if (g_agent_job) {
      TerminateJobObject(g_agent_job, 0);
    } else {
      TerminateProcess(g_agent_process, 0);
    }
    WaitForSingleObject(g_agent_process, 3000);
  }

  desklink::StopServiceAuthBroker();

  if (g_agent_job) {
    CloseHandle(g_agent_job);
    g_agent_job = nullptr;
  }
  if (g_agent_process) {
    CloseHandle(g_agent_process);
    g_agent_process = nullptr;
  }
  CloseAgentStopEvent();
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

  std::wstring protected_credential;
  std::wstring credential_error;
  const desklink::ProtectedCredentialStatus credential_status =
      desklink::LoadProtectedDeviceCredential(&protected_credential, &credential_error);
  if (credential_status == desklink::ProtectedCredentialStatus::Error) {
    if (has_environment) DestroyEnvironmentBlock(environment);
    CloseHandle(user_token);
    LogEvent(
        EVENTLOG_ERROR_TYPE,
        L"Protected device credential could not be loaded: " + credential_error);
    return false;
  }
  if (credential_status == desklink::ProtectedCredentialStatus::Loaded && !has_environment) {
    SecureWipe(&protected_credential);
    CloseHandle(user_token);
    LogEvent(
        EVENTLOG_ERROR_TYPE,
        L"CreateEnvironmentBlock failed; protected Service authentication requires a child environment block");
    return false;
  }

  std::vector<wchar_t> protected_environment;
  LPVOID child_environment = has_environment ? environment : nullptr;
  std::string broker_device_id;
  std::string broker_endpoint;
  std::string broker_credential;
  std::wstring auth_pipe_name;

  if (credential_status == desklink::ProtectedCredentialStatus::Loaded) {
    protected_environment = BuildEnvironmentWithoutDeviceCredential(environment);
    child_environment = protected_environment.data();

    broker_device_id = WideToUtf8(EnvironmentValue(L"DESKLINK_DEVICE_ID"));
    broker_endpoint = WideToUtf8(EnvironmentValue(L"DESKLINK_SIGNAL_TOKEN_URL"));
    broker_credential = WideToUtf8(protected_credential);
    auth_pipe_name = NewServiceAuthPipeName(session_id);
    if (broker_device_id.empty() || broker_endpoint.empty() || broker_credential.empty()) {
      SecureWipe(&protected_credential);
      SecureWipe(&broker_credential);
      SecureWipe(&protected_environment);
      if (has_environment) DestroyEnvironmentBlock(environment);
      CloseHandle(user_token);
      LogEvent(
          EVENTLOG_ERROR_TYPE,
          L"DPAPI device credential requires DESKLINK_DEVICE_ID and DESKLINK_SIGNAL_TOKEN_URL");
      return false;
    }
  }

  if (!CreateAgentStopEvent(session_id)) {
    SecureWipe(&protected_credential);
    SecureWipe(&broker_credential);
    SecureWipe(&protected_environment);
    if (has_environment) DestroyEnvironmentBlock(environment);
    CloseHandle(user_token);
    return false;
  }

  std::wstring command = L"\"" + agent_path.wstring() + L"\"";
  std::wstring output_index = OutputIndexArgument();
  if (output_index.empty()) output_index = L"0";
  command += L" " + output_index;
  command += L" --service-stop-event=\"" + g_agent_stop_event_name + L"\"";
  if (!auth_pipe_name.empty()) {
    command += L" --service-auth-pipe=\"" + auth_pipe_name + L"\"";
  }
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  std::wstring desktop = L"winsta0\\default";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.lpDesktop = desktop.data();

  PROCESS_INFORMATION process{};
  const bool broker_mode = credential_status == desklink::ProtectedCredentialStatus::Loaded;
  DWORD creation_flags = CREATE_NO_WINDOW;
  if (broker_mode) creation_flags |= CREATE_SUSPENDED;
  if (child_environment) creation_flags |= CREATE_UNICODE_ENVIRONMENT;

  const std::wstring working_directory = agent_path.parent_path().wstring();
  const BOOL created = CreateProcessAsUserW(
      user_token,
      agent_path.c_str(),
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      creation_flags,
      child_environment,
      working_directory.c_str(),
      &startup,
      &process);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

  if (!created) {
    SecureWipe(&protected_credential);
    SecureWipe(&broker_credential);
    SecureWipe(&protected_environment);
    if (has_environment) DestroyEnvironmentBlock(environment);
    CloseHandle(user_token);
    CloseAgentStopEvent();
    LogEvent(
        EVENTLOG_ERROR_TYPE,
        L"CreateProcessAsUser failed for session " + std::to_wstring(session_id) +
            L" (error " + std::to_wstring(create_error) + L")");
    return false;
  }

  // Put the still-suspended protected-mode Agent into its Job Object before any
  // user-mode code can run. Non-broker development mode is already running here,
  // but follows the same containment setup as before.
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

  bool broker_started = true;
  std::wstring broker_error;
  if (broker_mode) {
    broker_started = desklink::StartServiceAuthBroker(
        auth_pipe_name,
        process.dwProcessId,
        broker_endpoint,
        broker_device_id,
        std::move(broker_credential),
        &broker_error);
  }

  SecureWipe(&protected_credential);
  SecureWipe(&broker_credential);
  SecureWipe(&protected_environment);
  if (has_environment) DestroyEnvironmentBlock(environment);
  CloseHandle(user_token);

  if (!broker_started) {
    if (job) {
      TerminateJobObject(job, 0);
    } else {
      TerminateProcess(process.hProcess, 0);
    }
    WaitForSingleObject(process.hProcess, 3000);
    if (job) CloseHandle(job);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseAgentStopEvent();
    LogEvent(EVENTLOG_ERROR_TYPE, L"Unable to start local authentication broker: " + broker_error);
    return false;
  }

  if (broker_mode && ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
    const DWORD resume_error = GetLastError();
    desklink::StopServiceAuthBroker();
    if (job) {
      TerminateJobObject(job, 0);
    } else {
      TerminateProcess(process.hProcess, 0);
    }
    WaitForSingleObject(process.hProcess, 3000);
    if (job) CloseHandle(job);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseAgentStopEvent();
    LogEvent(
        EVENTLOG_ERROR_TYPE,
        L"Unable to resume broker-gated Agent process (error " +
            std::to_wstring(resume_error) + L")");
    return false;
  }

  CloseHandle(process.hThread);

  g_agent_process = process.hProcess;
  g_agent_job = job;
  g_agent_session = session_id;
  LogEvent(
      EVENTLOG_INFORMATION_TYPE,
      L"Started desklink-agent.exe in session " + std::to_wstring(session_id) +
          (broker_mode ? L" after Service authentication broker became ready" : L""));
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
  DWORD observed_active_session = kNoSession;

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
    const bool session_changed = active_session != observed_active_session;
    const bool agent_dead = g_agent_process && !AgentAlive();

    if (session_changed) {
      observed_active_session = active_session;
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
    if (command == L"--store-device-credential") return StoreDeviceCredentialCommand();
    if (command == L"--clear-device-credential") return ClearDeviceCredentialCommand();
    if (command != L"--service") {
      std::wcerr
          << L"Usage: desklink-service.exe --install | --uninstall | --service | "
             L"--store-device-credential | --clear-device-credential\n";
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
