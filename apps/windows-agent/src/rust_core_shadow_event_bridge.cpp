#include "rust_core_shadow.h"

namespace desklink {

bool RustCoreShadowEventBridge::available() const noexcept {
  return lifecycle_.available();
}

std::uint64_t RustCoreShadowEventBridge::mismatch_count() const {
  return lifecycle_.mismatch_count();
}

void RustCoreShadowEventBridge::ClearPendingLocked() {
  pending_control_generation_ = 0;
  pending_pointer_generation_ = 0;
}

RustCoreShadowPeerScope RustCoreShadowEventBridge::BeginPeer(
    bool same_authoritative_session) {
  std::scoped_lock lock(mutex_);
  current_scope_ = lifecycle_.BeginPeer(same_authoritative_session);
  peer_connected_ = false;
  ClearPendingLocked();
  return current_scope_;
}

bool RustCoreShadowEventBridge::ComparePeerConnected(
    RustCoreShadowPeerScope scope,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  bool matches = lifecycle_.ComparePeerConnected(scope, cpp_authoritative);
  if (!cpp_authoritative || scope != current_scope_ || !matches) {
    return matches;
  }

  peer_connected_ = true;
  const std::uint64_t pending_control = pending_control_generation_;
  const std::uint64_t pending_pointer = pending_pointer_generation_;
  ClearPendingLocked();

  if (pending_control != 0) {
    matches = lifecycle_.CompareControlOpened(scope, pending_control, true) && matches;
  }
  if (pending_pointer != 0) {
    matches = lifecycle_.ComparePointerOpened(scope, pending_pointer, true) && matches;
  }
  return matches;
}

bool RustCoreShadowEventBridge::CompareControlOpened(
    RustCoreShadowPeerScope scope,
    std::uint64_t control_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative && scope == current_scope_ && !peer_connected_) {
    pending_control_generation_ = control_generation;
    return true;
  }
  return lifecycle_.CompareControlOpened(
      scope,
      control_generation,
      cpp_authoritative);
}

bool RustCoreShadowEventBridge::CompareControlClosed(
    RustCoreShadowPeerScope scope,
    std::uint64_t control_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative && scope == current_scope_ && !peer_connected_ &&
      pending_control_generation_ == control_generation) {
    pending_control_generation_ = 0;
    return true;
  }
  return lifecycle_.CompareControlClosed(
      scope,
      control_generation,
      cpp_authoritative);
}

bool RustCoreShadowEventBridge::ComparePointerOpened(
    RustCoreShadowPeerScope scope,
    std::uint64_t pointer_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative && scope == current_scope_ && !peer_connected_) {
    pending_pointer_generation_ = pointer_generation;
    return true;
  }
  return lifecycle_.ComparePointerOpened(
      scope,
      pointer_generation,
      cpp_authoritative);
}

bool RustCoreShadowEventBridge::ComparePointerClosed(
    RustCoreShadowPeerScope scope,
    std::uint64_t pointer_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative && scope == current_scope_ && !peer_connected_ &&
      pending_pointer_generation_ == pointer_generation) {
    pending_pointer_generation_ = 0;
    return true;
  }
  return lifecycle_.ComparePointerClosed(
      scope,
      pointer_generation,
      cpp_authoritative);
}

bool RustCoreShadowEventBridge::EndSession() {
  std::scoped_lock lock(mutex_);
  const bool matches = lifecycle_.EndSession();
  current_scope_ = {};
  peer_connected_ = false;
  ClearPendingLocked();
  return matches;
}

}  // namespace desklink
