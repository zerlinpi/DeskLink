#pragma once

#include <atomic>
#include <string>

namespace desklink {

class InputInjector {
 public:
  void SetDesktopRect(long left, long top, long width, long height);
  bool PointerMove(double normalized_x, double normalized_y) const;
  bool PointerButton(int button, bool down) const;
  bool PointerWheel(int delta) const;
  bool Key(const std::string& code, bool down) const;

 private:
  static unsigned short VirtualKeyFromCode(const std::string& code);

  std::atomic<long> desktop_left_{0};
  std::atomic<long> desktop_top_{0};
  std::atomic<long> desktop_width_{0};
  std::atomic<long> desktop_height_{0};
};

}  // namespace desklink
