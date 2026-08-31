#pragma once

#include <cstdint>
#include <mutex>

namespace desklink {

// Prototype-only observer for the deterministic Rust session core.
//
// The C++ WebRtcSession remains authoritative. Each Observe* call mirrors a
// lifecycle decision that C++ already made and returns false only when the Rust
// status/command differs from the expected C++ decision. The caller may log or
// count that mismatch, but must never change production behavior because of it.
//
// libdatachannel may deliver peer/DataChannel callbacks on different threads.
// Observe* calls therefore serialize access to the single opaque Rust handle.
class RustCoreShadow {
 public:
  RustCoreShadow();
  ~RustCoreShadow();

  RustCoreShadow(const RustCoreShadow&) = delete;
  RustCoreShadow& operator=(const RustCoreShadow&) = delete;
  RustCoreShadow(RustCoreShadow&&) = delete;
  RustCoreShadow& operator=(RustCoreShadow&&) = delete;

  bool available() const noexcept;
  std::uint64_t mismatch_count() const;
  bool Reset();

  bool ObserveStart(std::uint64_t session);
  bool ObserveStaleStart(std::uint64_t session);
  bool ObserveSignalConnected(std::uint64_t session);
  bool ObserveAuthenticationAccepted(std::uint64_t session, std::uint64_t peer);
  bool ObservePeerConnected(std::uint64_t session, std::uint64_t peer);
  bool ObserveStalePeerConnected(std::uint64_t session, std::uint64_t peer);
  bool ObservePeerReplaced(std::uint64_t session, std::uint64_t peer);
  bool ObserveControlOpened(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t control);
  bool ObserveControlClosed(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t control);
  bool ObserveStaleControlOpened(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t control);
  bool ObserveStaleControlClosed(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t control);
  bool ObservePointerOpened(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t pointer);
  bool ObservePointerClosed(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t pointer);
  bool ObserveStalePointerOpened(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t pointer);
  bool ObserveStalePointerClosed(
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t pointer);
  bool ObserveOperationStarted(std::uint64_t session, std::uint64_t operation);
  bool ObserveOperationTimedOut(std::uint64_t session, std::uint64_t operation);
  bool ObserveStaleOperationStarted(std::uint64_t session, std::uint64_t operation);
  bool ObserveCloseRequested(std::uint64_t session);
  bool ObserveClosed(std::uint64_t session);

 private:
  bool Observe(
      std::uint32_t kind,
      std::uint64_t session,
      std::uint64_t peer,
      std::uint64_t control,
      std::uint64_t pointer,
      std::uint64_t operation,
      std::int32_t expected_status,
      std::uint32_t expected_command);

  mutable std::mutex mutex_;
  void* handle_{nullptr};
  std::uint64_t mismatch_count_{0};
};

struct RustCoreShadowPeerScope {
  std::uint64_t session{0};
  std::uint64_t peer{0};

  [[nodiscard]] bool valid() const noexcept {
    return session != 0 && peer != 0;
  }

  friend bool operator==(
      const RustCoreShadowPeerScope&,
      const RustCoreShadowPeerScope&) = default;
};

// Converts production C++ authority decisions into the normalized lifecycle
// understood by RustCoreShadow. This adapter deliberately does not decide which
// peer or input channel is authoritative: WebRtcSession and InputChannelAuthority
// remain the source of truth and pass their accepted/rejected result here.
class RustCoreShadowLifecycle {
 public:
  bool available() const noexcept;
  std::uint64_t mismatch_count() const;

  // same_authoritative_session=true means C++ replaced the peer while keeping
  // the same remote-control session. false closes/abandons any previous shadow
  // session and starts a new monotonically generated Session scope.
  RustCoreShadowPeerScope BeginPeer(bool same_authoritative_session);

  bool ComparePeerConnected(
      RustCoreShadowPeerScope scope,
      bool cpp_authoritative);
  bool CompareControlOpened(
      RustCoreShadowPeerScope scope,
      std::uint64_t control_generation,
      bool cpp_authoritative);
  bool CompareControlClosed(
      RustCoreShadowPeerScope scope,
      std::uint64_t control_generation,
      bool cpp_authoritative);
  bool ComparePointerOpened(
      RustCoreShadowPeerScope scope,
      std::uint64_t pointer_generation,
      bool cpp_authoritative);
  bool ComparePointerClosed(
      RustCoreShadowPeerScope scope,
      std::uint64_t pointer_generation,
      bool cpp_authoritative);

  // Ends the current normalized session. Connected sessions use the modeled
  // CloseRequested -> Closed path. A pre-connected session is observer-only and
  // is reset rather than inventing a production transition that C++ never made.
  bool EndSession();

 private:
  static std::uint64_t AdvanceNonZero(std::uint64_t* sequence);
  bool EndSessionLocked();

  mutable std::mutex mutex_;
  RustCoreShadow shadow_;
  std::uint64_t session_sequence_{0};
  std::uint64_t peer_sequence_{0};
  bool session_active_{false};
  bool peer_connected_{false};
  RustCoreShadowPeerScope current_scope_{};
};

}  // namespace desklink
