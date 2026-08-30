#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace desklink {

enum class PointerWireKind {
  Move,
  Wheel,
};

struct PointerWireEvent {
  PointerWireKind kind{PointerWireKind::Move};
  double x{0.0};
  double y{0.0};
  int wheel_delta{0};
};

bool ParsePointerWire(
    std::span<const std::byte> payload,
    PointerWireEvent* event);

// Temporary compatibility path for Web controllers that still send the old
// JSON pointer messages. It intentionally accepts only move and wheel events
// so the unreliable pointer channel cannot invoke reliable/privileged control.
bool ParseLegacyPointerJson(
    const std::string& text,
    PointerWireEvent* event);

}  // namespace desklink
