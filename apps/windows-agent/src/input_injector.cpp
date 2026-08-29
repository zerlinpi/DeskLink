#include "input_injector.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

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

LONG NormalizeVirtualCoordinate(double pixel, LONG origin, LONG extent) {
  if (extent <= 1) return 0;
  const double normalized = (pixel - static_cast<double>(origin)) /
                            static_cast<double>(extent - 1);
  return static_cast<LONG>(std::lround(std::clamp(normalized, 0.0, 1.0) * 65535.0));
}

}  // namespace

void InputInjector::SetDesktopRect(long left, long top, long width, long height) {
  desktop_left_.store(left, std::memory_order_relaxed);
  desktop_top_.store(top, std::memory_order_relaxed);
  desktop_width_.store(std::max<long>(0, width), std::memory_order_relaxed);
  desktop_height_.store(std::max<long>(0, height), std::memory_order_relaxed);
}

bool InputInjector::PointerMove(double normalized_x, double normalized_y) const {
  normalized_x = std::clamp(normalized_x, 0.0, 1.0);
  normalized_y = std::clamp(normalized_y, 0.0, 1.0);

  LONG virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  LONG virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  LONG virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  LONG virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtual_width <= 0 || virtual_height <= 0) {
    virtual_left = 0;
    virtual_top = 0;
    virtual_width = std::max<LONG>(1, GetSystemMetrics(SM_CXSCREEN));
    virtual_height = std::max<LONG>(1, GetSystemMetrics(SM_CYSCREEN));
  }

  LONG target_left = desktop_left_.load(std::memory_order_relaxed);
  LONG target_top = desktop_top_.load(std::memory_order_relaxed);
  LONG target_width = desktop_width_.load(std::memory_order_relaxed);
  LONG target_height = desktop_height_.load(std::memory_order_relaxed);
  if (target_width <= 0 || target_height <= 0) {
    target_left = virtual_left;
    target_top = virtual_top;
    target_width = virtual_width;
    target_height = virtual_height;
  }

  const double pixel_x = static_cast<double>(target_left) +
                         normalized_x * static_cast<double>(std::max<LONG>(0, target_width - 1));
  const double pixel_y = static_cast<double>(target_top) +
                         normalized_y * static_cast<double>(std::max<LONG>(0, target_height - 1));

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = NormalizeVirtualCoordinate(pixel_x, virtual_left, virtual_width);
  input.mi.dy = NormalizeVirtualCoordinate(pixel_y, virtual_top, virtual_height);
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  return Send(input);
}

bool InputInjector::PointerButton(int button, bool down) const {
  const DWORD flag = MouseButtonFlag(button, down);
  if (flag == 0) return false;

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flag;
  if (!Send(input)) return false;

  std::scoped_lock lock(pressed_mutex_);
  if (down) {
    pressed_buttons_.insert(button);
  } else {
    pressed_buttons_.erase(button);
  }
  return true;
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
  if (!Send(input)) return false;

  std::scoped_lock lock(pressed_mutex_);
  if (down) {
    pressed_keys_.insert(vk);
  } else {
    pressed_keys_.erase(vk);
  }
  return true;
}

bool InputInjector::ReleaseAll() const {
  std::vector<unsigned short> keys;
  std::vector<int> buttons;
  {
    std::scoped_lock lock(pressed_mutex_);
    keys.assign(pressed_keys_.begin(), pressed_keys_.end());
    buttons.assign(pressed_buttons_.begin(), pressed_buttons_.end());
    pressed_keys_.clear();
    pressed_buttons_.clear();
  }

  bool all_released = true;
  for (const unsigned short vk : keys) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (IsExtendedKey(vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    all_released = Send(input) && all_released;
  }

  for (const int button : buttons) {
    const DWORD flag = MouseButtonFlag(button, false);
    if (flag == 0) continue;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    all_released = Send(input) && all_released;
  }

  return all_released;
}

}  // namespace desklink
