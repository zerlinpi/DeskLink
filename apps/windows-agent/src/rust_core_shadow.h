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

  bool ObserveStart(std::uint64_t session);
  bool ObserveStaleStart(std::uint64_t session);
  bool ObserveSignalConnected(std::uint64_t session);
  bool ObserveAuthenticationAccepted(std::uint64_t session, std::uint64_t peer);
  bool ObservePeerConnected(std::uint64_t session, std::uint64_t peer);
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

}  // namespace desklink
