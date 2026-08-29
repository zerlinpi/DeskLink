#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <string>
#include <thread>

#include "input_injector.h"

namespace {
using namespace std::chrono_literals;

std::wstring ServiceStopEventName() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return {};

  constexpr wchar_t kPrefix[] = L"--service-stop-event=";
  constexpr size_t kPrefixLength = (sizeof(kPrefix) / sizeof(kPrefix[0])) - 1;
  std::wstring result;
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument(argv[i]);
    if (argument.rfind(kPrefix, 0) == 0) {
      result = argument.substr(kPrefixLength);
      break;
    }
  }
  LocalFree(argv);
  return result;
}

class ServiceShutdownBridge {
 public:
  ServiceShutdownBridge() {
    const std::wstring event_name = ServiceStopEventName();
    if (event_name.empty()) return;

    event_ = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
    if (!event_) return;

    worker_ = std::jthread([this](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        const DWORD wait = WaitForSingleObject(event_, 250);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0) return;

        // Release tracked remote input immediately. The normal Agent shutdown path
        // releases it again, which is intentionally idempotent.
        desklink::ReleaseAllInjectedInput();

        // The service launches the Agent with CREATE_NO_WINDOW, so allocate a
        // private console only for the shutdown notification. main.cpp already
        // handles CTRL_BREAK_EVENT by leaving its capture loop and running normal
        // WebRTC/DXGI/MF cleanup.
        if (GetConsoleOutputCP() == 0) {
          if (AllocConsole()) {
            if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
          }
        }

        if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0)) {
          // Input is already released. Exit only as a last local fallback; the
          // service also has a five-second Job Object kill timeout.
          ExitProcess(0);
        }
        return;
      }
    });
  }

  ~ServiceShutdownBridge() {
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
    if (event_) {
      CloseHandle(event_);
      event_ = nullptr;
    }
  }

 private:
  HANDLE event_{nullptr};
  std::jthread worker_;
};

ServiceShutdownBridge g_service_shutdown_bridge;

}  // namespace
