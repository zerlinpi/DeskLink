#include "input_injector.h"

#include <windows.h>

#include <algorithm>
#include <unordered_map>

namespace desklink {
namespace {

bool Send(INPUT& input) {
  return SendInput(1, &input, sizeof(INPUT)) == 1;
}

DWORD MouseButtonFlag(int button, bool down) {
  switch (button) {
    case 0:
      return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    case 1:
      return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    case 2:
      return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    default:
      return 0;
  }
}

}  // namespace

bool InputInjector::PointerMove(double normalized_x, double normalized_y) const {
  normalized_x = std::clamp(normalized_x, 0.0, 1.0);
  normalized_y = std::clamp(normalized_y, 0.0, 1.0);

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = static_cast<LONG>(normalized_x * 65535.0);
  input.mi.dy = static_cast<LONG>(normalized_y * 65535.0);
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  return Send(input);
}

bool InputInjector::PointerButton(int button, bool down) const {
  const DWORD flag = MouseButtonFlag(button, down);
  if (flag == 0) return false;

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flag;
  return Send(input);
}

bool InputInjector::PointerWheel(int delta) const {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.mouseData = static_cast<DWORD>(delta);
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  return Send(input);
}

unsigned short InputInjector::VirtualKeyFromCode(const std::string& code) {
  if (code.size() == 4 && code.rfind("Key", 0) == 0) {
    const char c = code[3];
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned short>(c);
  }
  if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
    const char c = code[5];
    if (c >= '0' && c <= '9') return static_cast<unsigned short>(c);
  }
  if (code.size() >= 2 && code[0] == 'F') {
    try {
      const int n = std::stoi(code.substr(1));
      if (n >= 1 && n <= 24) return static_cast<unsigned short>(VK_F1 + (n - 1));
    } catch (...) {
    }
  }

  static const std::unordered_map<std::string, unsigned short> kKeys = {
      {"Enter", VK_RETURN},       {"Escape", VK_ESCAPE},       {"Backspace", VK_BACK},
      {"Tab", VK_TAB},           {"Space", VK_SPACE},         {"Delete", VK_DELETE},
      {"Insert", VK_INSERT},     {"Home", VK_HOME},           {"End", VK_END},
      {"PageUp", VK_PRIOR},      {"PageDown", VK_NEXT},       {"ArrowLeft", VK_LEFT},
      {"ArrowRight", VK_RIGHT},  {"ArrowUp", VK_UP},          {"ArrowDown", VK_DOWN},
      {"ShiftLeft", VK_LSHIFT},  {"ShiftRight", VK_RSHIFT},   {"ControlLeft", VK_LCONTROL},
      {"ControlRight", VK_RCONTROL}, {"AltLeft", VK_LMENU},   {"AltRight", VK_RMENU},
      {"MetaLeft", VK_LWIN},     {"MetaRight", VK_RWIN},      {"CapsLock", VK_CAPITAL},
      {"NumLock", VK_NUMLOCK},   {"ScrollLock", VK_SCROLL},   {"PrintScreen", VK_SNAPSHOT},
      {"Pause", VK_PAUSE},       {"NumpadAdd", VK_ADD},       {"NumpadSubtract", VK_SUBTRACT},
      {"NumpadMultiply", VK_MULTIPLY}, {"NumpadDivide", VK_DIVIDE}, {"NumpadDecimal", VK_DECIMAL},
  };

  const auto it = kKeys.find(code);
  return it == kKeys.end() ? 0 : it->second;
}

bool InputInjector::Key(const std::string& code, bool down) const {
  const unsigned short vk = VirtualKeyFromCode(code);
  if (vk == 0) return false;

  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  return Send(input);
}

}  // namespace desklink
