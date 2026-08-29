#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "service_auth_broker.h"

namespace {
using namespace std::chrono_literals;

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

int UnauthorizedChild(const std::wstring& pipe_name) {
  ScopedKernelHandle pipe(ConnectPipeWithRetry(pipe_name));
  if (!pipe) return 20;

  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return 21;

  // The server checks client PID immediately after connection, before it reads a
  // command. This child intentionally has the same Windows user SID as the parent
  // but a different process ID, so it must be rejected by the PID boundary.
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

bool RunUnauthorizedChildProcess(const std::wstring& pipe_name) {
  std::vector<wchar_t> module(32768);
  const DWORD length = GetModuleFileNameW(
      nullptr,
      module.data(),
      static_cast<DWORD>(module.size()));
  if (length == 0 || length >= module.size()) return false;

  const std::wstring executable(module.data(), length);
  std::wstring command =
      L"\"" + executable + L"\" --unauthorized-child \"" + pipe_name + L"\"";
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
          nullptr,
          &startup,
          &process)) {
    return false;
  }

  CloseHandle(process.hThread);
  ScopedKernelHandle child(process.hProcess);
  if (WaitForSingleObject(child.get(), 10000) != WAIT_OBJECT_0) {
    TerminateProcess(child.get(), 24);
    return false;
  }

  DWORD exit_code = 0;
  return GetExitCodeProcess(child.get(), &exit_code) && exit_code == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--unauthorized-child") {
    return UnauthorizedChild(argv[2]);
  }

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\DeskLink.Auth.Smoke." + std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());

  std::wstring error;
  if (!desklink::StartServiceAuthBroker(
          pipe_name,
          GetCurrentProcessId(),
          "http://127.0.0.1:1/api/v1/signal-token",
          "ci-smoke-device",
          "dc1.ci-smoke-placeholder",
          &error)) {
    std::wcerr << L"Unable to start auth broker: " << error << L"\n";
    return 1;
  }

  {
    // StartServiceAuthBroker promises that the first pipe instance already exists.
    // Deliberately do not retry here; a race in broker readiness should fail CI.
    ScopedKernelHandle pipe(CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!pipe) {
      std::wcerr << L"Broker returned before its first pipe instance was ready\n";
      desklink::StopServiceAuthBroker();
      return 2;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
      std::wcerr << L"Unable to set message read mode\n";
      desklink::StopServiceAuthBroker();
      return 3;
    }

    constexpr char request[] = "unsupported-smoke-command";
    DWORD written = 0;
    if (!WriteFile(
            pipe.get(),
            request,
            static_cast<DWORD>(sizeof(request) - 1),
            &written,
            nullptr) ||
        written != sizeof(request) - 1) {
      std::wcerr << L"Unable to send smoke request\n";
      desklink::StopServiceAuthBroker();
      return 4;
    }

    char response[2048]{};
    DWORD read = 0;
    if (!ReadFile(pipe.get(), response, sizeof(response) - 1, &read, nullptr) || read == 0) {
      std::wcerr << L"Unable to read broker smoke response\n";
      desklink::StopServiceAuthBroker();
      return 5;
    }

    const std::string body(response, read);
    if (body.find("unsupported request") == std::string::npos ||
        body.find("\"ok\":false") == std::string::npos) {
      std::cerr << "Unexpected broker smoke response: " << body << "\n";
      desklink::StopServiceAuthBroker();
      return 6;
    }
  }

  if (!RunUnauthorizedChildProcess(pipe_name)) {
    std::wcerr << L"Broker did not reject a same-user process with the wrong PID\n";
    desklink::StopServiceAuthBroker();
    return 7;
  }

  desklink::StopServiceAuthBroker();
  std::cout << "Service auth broker smoke test passed\n";
  return 0;
}
