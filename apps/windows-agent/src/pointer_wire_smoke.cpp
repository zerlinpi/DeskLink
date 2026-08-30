#include "pointer_wire.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace {

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value & 0xffu));
  }
  return result;
}

bool Near(double left, double right) {
  return std::abs(left - right) < 0.00002;
}

}  // namespace

int main() {
  desklink::PointerWireEvent event;

  const auto move = Bytes({0xd1, 1, 1, 0x00, 0x80, 0xff, 0xff});
  if (!desklink::ParsePointerWire(move, &event) ||
      event.kind != desklink::PointerWireKind::Move ||
      !Near(event.x, 32768.0 / 65535.0) || !Near(event.y, 1.0)) {
    std::cerr << "Binary pointer move vector failed.\n";
    return 1;
  }

  const auto wheel = Bytes({0xd1, 1, 2, 0xa8, 0xfd});
  if (!desklink::ParsePointerWire(wheel, &event) ||
      event.kind != desklink::PointerWireKind::Wheel ||
      event.wheel_delta != -600) {
    std::cerr << "Binary pointer wheel vector failed.\n";
    return 1;
  }

  for (const auto& invalid : {
           Bytes({0xd0, 1, 1, 0, 0, 0, 0}),
           Bytes({0xd1, 2, 1, 0, 0, 0, 0}),
           Bytes({0xd1, 1, 9, 0, 0}),
           Bytes({0xd1, 1, 1, 0, 0}),
       }) {
    if (desklink::ParsePointerWire(invalid, &event)) {
      std::cerr << "Invalid binary pointer payload was accepted.\n";
      return 1;
    }
  }

  if (!desklink::ParseLegacyPointerJson(
          R"({"t":"pointer","kind":"move","x":0.25,"y":0.75})",
          &event) ||
      event.kind != desklink::PointerWireKind::Move ||
      !Near(event.x, 0.25) || !Near(event.y, 0.75)) {
    std::cerr << "Legacy pointer move compatibility failed.\n";
    return 1;
  }

  if (!desklink::ParseLegacyPointerJson(R"({"t":"wheel","delta":120})", &event) ||
      event.kind != desklink::PointerWireKind::Wheel || event.wheel_delta != 120) {
    std::cerr << "Legacy wheel compatibility failed.\n";
    return 1;
  }

  if (desklink::ParseLegacyPointerJson(
          R"({"t":"pointer","kind":"down","x":0.5,"y":0.5,"button":0})",
          &event) ||
      desklink::ParseLegacyPointerJson(
          R"({"t":"system-operation","operation":"secure-attention-sequence"})",
          &event)) {
    std::cerr << "Unreliable pointer channel accepted a reliable/privileged operation.\n";
    return 1;
  }

  std::cout << "DeskLink binary pointer wire smoke passed.\n";
  return 0;
}
