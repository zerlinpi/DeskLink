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

bool IsExtendedKey(unsigned short vk) {
  switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_SNAPSHOT:
    case VK_DIVIDE:
    case VK_LWIN:
    case VK_RWIN:
      return true;
    default:
      return false;
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
  if (code.size() == 7 && code.rfind("Numpad", 0) == 0) {
    const char c = code[6];
    if (c >= '0' && c <= '9') return static_cast<unsigned short>(VK_NUMPAD0 + (c - '0'));
  }
  if (code.size() >= 2 && code[0] == 'F') {
    try {
      const int n = std::stoi(code.substr(1));
      if (n >= 1 && n <= 24) return static_cast<unsigned short>(VK_F1 + (n - 1));
    } catch (...) {
    }
  }

  static const std::unordered_map<std::string, int> kKeys = {
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
      {"NumpadEnter", VK_RETURN}, {"Semicolon", VK_OEM_1},    {"Equal", VK_OEM_PLUS},
      {"Comma", VK_OEM_COMMA},   {"Minus", VK_OEM_MINUS},    {"Period", VK_OEM_PERIOD},
      {"Slash", VK_OEM_2},       {"Backquote", VK_OEM_3},    {"BracketLeft", VK_OEM_4},
      {"Backslash", VK_OEM_5},   {"BracketRight", VK_OEM_6}, {"Quote", VK_OEM_7},
  };

  const auto it = kKeys.find(code);
  return it == kKeys.end() ? 0 : static_cast<unsigned short>(it->second);
}

bool InputInjector::Key(const std::string& code, bool down) const {
  const unsigned short vk = VirtualKeyFromCode(code);
  if (vk == 0) return false;

  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (IsExtendedKey(vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  return Send(input);
}

}  // namespace desklink
