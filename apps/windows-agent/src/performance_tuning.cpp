#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>

#include <cstdlib>
#include <string>

namespace desklink {
namespace {

bool PerformanceTuningEnabled() {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "DESKLINK_PERFORMANCE_TUNING") != 0 || value == nullptr) {
    if (value) std::free(value);
    return true;
  }

  const std::string setting(value);
  std::free(value);
  return setting != "0" && setting != "false" && setting != "off";
}

class WindowsPerformanceTuning {
 public:
  WindowsPerformanceTuning() {
    if (!PerformanceTuningEnabled()) return;

    // Remote desktop should stay responsive under background CPU load without
    // taking the risks of REALTIME_PRIORITY_CLASS.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // libdatachannel's RTP pacer uses millisecond-scale scheduling. A 1 ms timer
    // period also reduces wake-up variance for the capture/encode loop on Windows.
    timer_period_enabled_ = timeBeginPeriod(1) == TIMERR_NOERROR;

    DWORD task_index = 0;
    mmcss_handle_ = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
    if (!mmcss_handle_) {
      task_index = 0;
      mmcss_handle_ = AvSetMmThreadCharacteristicsW(L"Games", &task_index);
    }
    if (mmcss_handle_) {
      AvSetMmThreadPriority(mmcss_handle_, AVRT_PRIORITY_HIGH);
    }
  }

  ~WindowsPerformanceTuning() {
    if (mmcss_handle_) {
      AvRevertMmThreadCharacteristics(mmcss_handle_);
      mmcss_handle_ = nullptr;
    }
    if (timer_period_enabled_) {
      timeEndPeriod(1);
    }
  }

  WindowsPerformanceTuning(const WindowsPerformanceTuning&) = delete;
  WindowsPerformanceTuning& operator=(const WindowsPerformanceTuning&) = delete;

 private:
  HANDLE mmcss_handle_{nullptr};
  bool timer_period_enabled_{false};
};

// This translation unit is linked directly into desklink-agent. CRT static
// initialization runs on the primary thread before wmain(), so the MMCSS task
// applies to the thread that later performs DXGI capture and hardware encoding.
WindowsPerformanceTuning g_windows_performance_tuning;

}  // namespace
}  // namespace desklink
