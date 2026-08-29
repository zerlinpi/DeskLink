#pragma once

#include <string>

namespace desklink {

class InputInjector {
 public:
  bool PointerMove(double normalized_x, double normalized_y) const;
  bool PointerButton(int button, bool down) const;
  bool PointerWheel(int delta) const;
  bool Key(const std::string& code, bool down) const;

 private:
  static unsigned short VirtualKeyFromCode(const std::string& code);
};

}  // namespace desklink
