#pragma once

#include <atomic>
#include <string>

namespace desklink {

// Releases every key/button currently held down by DeskLink in this Agent process.
// This is process-wide so shutdown paths outside WebRtcSession can safely prevent
// stuck remote input before the process exits.
bool ReleaseAllInjectedInput();

class InputInjector {
 public:
  void SetDesktopRect(long left, long top, long width, long height);
  bool PointerMove(double normalized_x, double normalized_y) const;
  bool PointerButton(int button, bool down) const;
  bool PointerWheel(int delta) const;
  bool Key(const std::string& code, bool down) const;
  bool ReleaseAll() const;

 private:
  static unsigned short VirtualKeyFromCode(const std::string& code);

  std::atomic<long> desktop_left_{0};
  std::atomic<long> desktop_top_{0};
  std::atomic<long> desktop_width_{0};
  std::atomic<long> desktop_height_{0};
};

}  // namespace desklink
