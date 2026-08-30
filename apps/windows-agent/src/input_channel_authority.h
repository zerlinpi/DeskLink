#pragma once

#include <cstdint>

namespace desklink {

enum class InputChannelKind : uint8_t {
  Control = 0,
  Pointer = 1,
};

using InputChannelGeneration = uint64_t;

// Tracks which logical input DataChannel is allowed to affect the active
// remote-control session. Replacements receive a fresh generation so delayed
// callbacks from an older SCTP channel become harmless immediately.
class InputChannelAuthority {
 public:
  InputChannelGeneration Activate(InputChannelKind kind) {
    auto& sequence = Sequence(kind);
    ++sequence;
    if (sequence == 0) ++sequence;
    Active(kind) = sequence;
    return sequence;
  }

  [[nodiscard]] bool IsCurrent(
      InputChannelKind kind,
      InputChannelGeneration generation) const {
    return generation != 0 && Active(kind) == generation;
  }

  bool RevokeIfCurrent(
      InputChannelKind kind,
      InputChannelGeneration generation) {
    if (!IsCurrent(kind, generation)) return false;
    Active(kind) = 0;
    return true;
  }

  void InvalidateAll() {
    active_control_ = 0;
    active_pointer_ = 0;
  }

 private:
  InputChannelGeneration& Sequence(InputChannelKind kind) {
    return kind == InputChannelKind::Control ? control_sequence_ : pointer_sequence_;
  }

  InputChannelGeneration& Active(InputChannelKind kind) {
    return kind == InputChannelKind::Control ? active_control_ : active_pointer_;
  }

  [[nodiscard]] const InputChannelGeneration& Active(InputChannelKind kind) const {
    return kind == InputChannelKind::Control ? active_control_ : active_pointer_;
  }

  InputChannelGeneration control_sequence_{0};
  InputChannelGeneration pointer_sequence_{0};
  InputChannelGeneration active_control_{0};
  InputChannelGeneration active_pointer_{0};
};

}  // namespace desklink
