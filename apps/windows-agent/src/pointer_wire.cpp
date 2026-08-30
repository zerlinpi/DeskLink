#include "pointer_wire.h"

#include <algorithm>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace desklink {
namespace {

constexpr uint8_t kMagic = 0xd1;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kMove = 1;
constexpr uint8_t kWheel = 2;
constexpr size_t kMoveSize = 7;
constexpr size_t kWheelSize = 5;
constexpr size_t kMaxLegacyPointerJsonBytes = 256;

uint8_t Byte(std::byte value) {
  return std::to_integer<uint8_t>(value);
}

uint16_t ReadU16Le(std::span<const std::byte> payload, size_t offset) {
  return static_cast<uint16_t>(Byte(payload[offset])) |
         static_cast<uint16_t>(static_cast<uint16_t>(Byte(payload[offset + 1])) << 8);
}

}  // namespace

bool ParsePointerWire(
    std::span<const std::byte> payload,
    PointerWireEvent* event) {
  if (!event || payload.size() < 3 ||
      Byte(payload[0]) != kMagic || Byte(payload[1]) != kVersion) {
    return false;
  }

  const uint8_t opcode = Byte(payload[2]);
  if (opcode == kMove) {
    if (payload.size() != kMoveSize) return false;
    const uint16_t raw_x = ReadU16Le(payload, 3);
    const uint16_t raw_y = ReadU16Le(payload, 5);
    event->kind = PointerWireKind::Move;
    event->x = static_cast<double>(raw_x) / 65535.0;
    event->y = static_cast<double>(raw_y) / 65535.0;
    event->wheel_delta = 0;
    return true;
  }

  if (opcode == kWheel) {
    if (payload.size() != kWheelSize) return false;
    const uint16_t raw = ReadU16Le(payload, 3);
    const int delta = (raw & 0x8000u) != 0
        ? static_cast<int>(raw) - 0x10000
        : static_cast<int>(raw);
    event->kind = PointerWireKind::Wheel;
    event->x = 0.0;
    event->y = 0.0;
    event->wheel_delta = delta;
    return true;
  }

  return false;
}

bool ParseLegacyPointerJson(
    const std::string& text,
    PointerWireEvent* event) {
  if (!event || text.empty() || text.size() > kMaxLegacyPointerJsonBytes) return false;

  const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return false;

  const auto type_it = parsed.find("t");
  if (type_it == parsed.end() || !type_it->is_string()) return false;
  const std::string type = type_it->get<std::string>();

  if (type == "pointer") {
    const auto kind_it = parsed.find("kind");
    const auto x_it = parsed.find("x");
    const auto y_it = parsed.find("y");
    if (kind_it == parsed.end() || !kind_it->is_string() ||
        kind_it->get<std::string>() != "move" ||
        x_it == parsed.end() || !x_it->is_number() ||
        y_it == parsed.end() || !y_it->is_number()) {
      return false;
    }

    event->kind = PointerWireKind::Move;
    event->x = std::clamp(x_it->get<double>(), 0.0, 1.0);
    event->y = std::clamp(y_it->get<double>(), 0.0, 1.0);
    event->wheel_delta = 0;
    return true;
  }

  if (type == "wheel") {
    const auto delta_it = parsed.find("delta");
    if (delta_it == parsed.end() || !delta_it->is_number_integer()) return false;
    const int64_t delta = delta_it->get<int64_t>();
    event->kind = PointerWireKind::Wheel;
    event->x = 0.0;
    event->y = 0.0;
    event->wheel_delta = static_cast<int>(std::clamp<int64_t>(delta, -32768, 32767));
    return true;
  }

  return false;
}

}  // namespace desklink
