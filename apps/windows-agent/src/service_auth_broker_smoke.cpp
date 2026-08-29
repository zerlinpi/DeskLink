#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "service_auth_broker.h"
#include "service_auth_client.h"

namespace {
using namespace std::chrono_literals;

constexpr char kDeviceId[] = "ci-smoke-device";
constexpr char kDeviceCredential[] = "dc1.ci-smoke-placeholder";
constexpr char kExpectedSignalToken[] = "ci-short-signal-token";

class ScopedKernelHandle {
 public:
  explicit ScopedKernelHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedKernelHandle() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  ScopedKernelHandle(const ScopedKernelHandle&) = delete;
  ScopedKernelHandle& operator=(const ScopedKernelHandle&) = delete;
  HANDLE get() const { return handle_; }
  explicit operator bool() const {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

class MockSignalTokenServer {
 public:
  ~MockSignalTokenServer() { Stop(); }

  bool Start() {
    if (WSAStartup(MAKEWORD(2, 2), &wsa_) != 0) return false;
    wsa_started_ = true;

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(
            listener_,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR ||
        listen(listener_, 1) == SOCKET_ERROR) {
      return false;
    }

    int address_length = sizeof(address);
    if (getsockname(
            listener_,
            reinterpret_cast<sockaddr*>(&address),
            &address_length) == SOCKET_ERROR) {
      return false;
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::jthread([this](std::stop_token) { ServeOne(); });
    return port_ != 0;
  }

  void Stop() {
    if (listener_ != INVALID_SOCKET) {
      closesocket(listener_);
      listener_ = INVALID_SOCKET;
    }
    if (worker_.joinable()) worker_.join();
    if (wsa_started_) {
      WSACleanup();
      wsa_started_ = false;
    }
  }

  std::string Endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/api/v1/signal-token";
  }

  bool request_valid() const { return request_valid_.load(std::memory_order_relaxed); }

 private:
  void ServeOne() {
    SOCKET client = accept(listener_, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;

    DWORD timeout_ms = 10000;
    setsockopt(
        client,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));
    setsockopt(
        client,
        SOL_SOCKET,
        SO_SNDTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));

    std::string request;
    char buffer[2048];
    while (request.size() < 16 * 1024 && request.find("\r\n\r\n") == std::string::npos) {
      const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received <= 0) break;
      request.append(buffer, static_cast<size_t>(received));
    }

    const bool path_ok = request.find(
        std::string("GET /api/v1/signal-token?deviceId=") + kDeviceId + " ") !=
        std::string::npos;
    const bool credential_ok = request.find(
        std::string("Authorization: Bearer ") + kDeviceCredential + "\r\n") !=
        std::string::npos;
    request_valid_.store(path_ok && credential_ok, std::memory_order_relaxed);

    const int64_t expires_at = static_cast<int64_t>(std::time(nullptr)) + 900;
    const std::string body = path_ok && credential_ok
        ? std::string("{\"token\":\"") + kExpectedSignalToken +
              "\",\"expiresAt\":" + std::to_string(expires_at) + "}"
        : "{\"error\":\"unauthorized\"}";
    const std::string status = path_ok && credential_ok
        ? "HTTP/1.1 200 OK\r\n"
        : "HTTP/1.1 401 Unauthorized\r\n";
    const std::string response =
        status +
        "Content-Type: application/json\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    size_t offset = 0;
    while (offset < response.size()) {
      const int sent = send(
          client,
          response.data() + offset,
          static_cast<int>(response.size() - offset),
          0);
      if (sent <= 0) break;
      offset += static_cast<size_t>(sent);
    }

    shutdown(client, SD_BOTH);
    closesocket(client);
  }

  WSADATA wsa_{};
  bool wsa_started_{false};
  SOCKET listener_{INVALID_SOCKET};
  uint16_t port_{0};
  std::jthread worker_;
  std::atomic_bool request_valid_{false};
};

HANDLE ConnectPipeWithRetry(const std::wstring& pipe_name) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    HANDLE pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;

    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
      return INVALID_HANDLE_VALUE;
    }
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(pipe_name.c_str(), 250);
    } else {
      std::this_thread::sleep_for(50ms);
    }
  }
  return INVALID_HANDLE_VALUE;
}

bool ValidateUnsupportedCommandOnReadyPipe(const std::wstring& pipe_name) {
  // Do not retry this first connection. Broker startup promises that the first
  // pipe instance is already created before StartServiceAuthBroker returns.
  ScopedKernelHandle pipe(CreateFileW(
      pipe_name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!pipe) return false;

  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return false;

  constexpr char request[] = "unsupported-smoke-command";
  DWORD written = 0;
  if (!WriteFile(
          pipe.get(),
          request,
          static_cast<DWORD>(sizeof(request) - 1),
          &written,
          nullptr) ||
      written != sizeof(request) - 1) {
    return false;
  }

  char response[2048]{};
  DWORD read = 0;
  if (!ReadFile(pipe.get(), response, sizeof(response) - 1, &read, nullptr) || read == 0) {
    return false;
  }

  const std::string body(response, read);
  return body.find("unsupported request") != std::string::npos &&
         body.find("\"ok\":false") != std::string::npos;
}

int AuthorizedChild(const std::wstring& pipe_name) {
  if (!ValidateUnsupportedCommandOnReadyPipe(pipe_name)) {
    std::cerr << "Broker first pipe was not immediately ready or command validation failed\n";
    return 9;
  }

  // This second connection uses the same client helper compiled into
  // desklink-agent.exe. The --service-auth-pipe argument is parsed internally.
  desklink::RuntimeSignalToken token;
  std::string error;
  if (!desklink::FetchServiceBrokerSignalToken(&token, &error)) {
    std::cerr << "Unable to fetch broker token: " << error << "\n";
    return 10;
  }

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (token.token != kExpectedSignalToken || token.expires_at <= now + 300) {
    std::cerr << "Broker returned unexpected signal token\n";
    return 11;
  }
  return 0;
}

int UnauthorizedChild(const std::wstring& pipe_name) {
  ScopedKernelHandle pipe(ConnectPipeWithRetry(pipe_name));
  if (!pipe) return 20;

  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return 21;

  // This child has the same user SID as the authorized Agent child but a
  // different PID. The SID ACL therefore allows it to open the Pipe, while the
  // server-side PID identity check must still reject it before reading a command.
  char response[2048]{};
  DWORD read = 0;
  if (!ReadFile(pipe.get(), response, sizeof(response) - 1, &read, nullptr) || read == 0) {
    return 22;
  }

  const std::string body(response, read);
  return body.find("unauthorized local client") != std::string::npos &&
                 body.find("\"ok\":false") != std::string::npos
             ? 0
             : 23;
}

std::wstring CurrentExecutablePath() {
  std::vector<wchar_t> module(32768);
  const DWORD length = GetModuleFileNameW(
      nullptr,
      module.data(),
      static_cast<DWORD>(module.size()));
  if (length == 0 || length >= module.size()) return {};
  return std::wstring(module.data(), length);
}

bool RunChildProcess(
    const std::wstring& arguments,
    DWORD creation_flags,
    PROCESS_INFORMATION* process) {
  if (!process) return false;
  *process = {};
  const std::wstring executable = CurrentExecutablePath();
  if (executable.empty()) return false;

  std::wstring command = L"\"" + executable + L"\" " + arguments;
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  return CreateProcessW(
             executable.c_str(),
             command_line.data(),
             nullptr,
             nullptr,
             FALSE,
             creation_flags,
             nullptr,
             nullptr,
             &startup,
             process) == TRUE;
}

bool WaitForSuccessfulChild(PROCESS_INFORMATION* process, DWORD timeout_ms = 10000) {
  if (!process || !process->hProcess) return false;
  if (process->hThread) {
    CloseHandle(process->hThread);
    process->hThread = nullptr;
  }

  ScopedKernelHandle child(process->hProcess);
  process->hProcess = nullptr;
  if (WaitForSingleObject(child.get(), timeout_ms) != WAIT_OBJECT_0) {
    TerminateProcess(child.get(), 99);
    return false;
  }

  DWORD exit_code = 0;
  return GetExitCodeProcess(child.get(), &exit_code) && exit_code == 0;
}

bool RunUnauthorizedChildProcess(const std::wstring& pipe_name) {
  PROCESS_INFORMATION process{};
  if (!RunChildProcess(
          L"--unauthorized-child \"" + pipe_name + L"\"",
          CREATE_NO_WINDOW,
          &process)) {
    return false;
  }
  return WaitForSuccessfulChild(&process);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 3 && std::wstring(argv[1]) == L"--authorized-child") {
    return AuthorizedChild(argv[2]);
  }
  if (argc == 3 && std::wstring(argv[1]) == L"--unauthorized-child") {
    return UnauthorizedChild(argv[2]);
  }

  MockSignalTokenServer mock_server;
  if (!mock_server.Start()) {
    std::wcerr << L"Unable to start loopback Signal Token mock server\n";
    return 1;
  }

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\DeskLink.Auth.Smoke." + std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());

  // Create the authorized child suspended so the parent can bind the broker to
  // the exact child PID and make the Pipe ready before any Agent-side code runs.
  PROCESS_INFORMATION authorized{};
  if (!RunChildProcess(
          L"--authorized-child \"" + pipe_name + L"\" --service-auth-pipe=\"" +
              pipe_name + L"\"",
          CREATE_NO_WINDOW | CREATE_SUSPENDED,
          &authorized)) {
    std::wcerr << L"Unable to create authorized broker child\n";
    return 2;
  }

  std::wstring broker_error;
  if (!desklink::StartServiceAuthBroker(
          pipe_name,
          authorized.dwProcessId,
          mock_server.Endpoint(),
          kDeviceId,
          kDeviceCredential,
          &broker_error)) {
    TerminateProcess(authorized.hProcess, 3);
    CloseHandle(authorized.hThread);
    CloseHandle(authorized.hProcess);
    std::wcerr << L"Unable to start auth broker: " << broker_error << L"\n";
    return 3;
  }

  if (ResumeThread(authorized.hThread) == static_cast<DWORD>(-1)) {
    TerminateProcess(authorized.hProcess, 4);
    CloseHandle(authorized.hThread);
    CloseHandle(authorized.hProcess);
    desklink::StopServiceAuthBroker();
    return 4;
  }

  // The authorized child first proves synchronous pipe readiness and then uses
  // the production Agent-side client for Service -> HTTP -> Pipe token exchange.
  if (!WaitForSuccessfulChild(&authorized, 15000)) {
    std::wcerr << L"Authorized Agent-side broker token exchange failed\n";
    desklink::StopServiceAuthBroker();
    return 5;
  }

  mock_server.Stop();
  if (!mock_server.request_valid()) {
    std::wcerr << L"Broker did not present the expected device credential to the loopback server\n";
    desklink::StopServiceAuthBroker();
    return 6;
  }

  if (!RunUnauthorizedChildProcess(pipe_name)) {
    std::wcerr << L"Broker did not reject a same-user process with the wrong PID\n";
    desklink::StopServiceAuthBroker();
    return 7;
  }

  desklink::StopServiceAuthBroker();
  std::cout << "Service auth broker end-to-end smoke test passed\n";
  return 0;
}
