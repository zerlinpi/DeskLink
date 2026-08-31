#include "rust_core_shadow.h"

#ifndef DESKLINK_ENABLE_RUST_CORE_SHADOW
#define DESKLINK_ENABLE_RUST_CORE_SHADOW 0
#endif

#include <iostream>
#include <sstream>

#if DESKLINK_ENABLE_RUST_CORE_SHADOW
#include <windows.h>

#include "desklink_core.h"
#endif

namespace desklink {
namespace {

constexpr std::int32_t kStatusOk = 0;
constexpr std::int32_t kStatusStale = 3;

constexpr std::uint32_t kEventStart = 1;
constexpr std::uint32_t kEventSignalConnected = 2;
constexpr std::uint32_t kEventAuthenticationAccepted = 3;
constexpr std::uint32_t kEventPeerConnected = 4;
constexpr std::uint32_t kEventPeerReplaced = 5;
constexpr std::uint32_t kEventControlOpened = 6;
constexpr std::uint32_t kEventControlClosed = 7;
constexpr std::uint32_t kEventPointerOpened = 8;
constexpr std::uint32_t kEventPointerClosed = 9;
constexpr std::uint32_t kEventOperationStarted = 10;
constexpr std::uint32_t kEventOperationTimedOut = 11;
constexpr std::uint32_t kEventCloseRequested = 12;
constexpr std::uint32_t kEventClosed = 13;

constexpr std::uint32_t kCommandNone = 0;
constexpr std::uint32_t kCommandBeginSignaling = 1;
constexpr std::uint32_t kCommandBeginAuthentication = 2;
constexpr std::uint32_t kCommandBeginNegotiation = 3;
constexpr std::uint32_t kCommandSessionConnected = 4;
constexpr std::uint32_t kCommandBeginClose = 5;
constexpr std::uint32_t kCommandSessionClosed = 6;

void LogShadowMismatch(const std::string& message) {
  std::cerr << "Rust core shadow mismatch: " << message << '\n';
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  const std::string debugger_message = "DeskLink Rust core shadow mismatch: " + message + "\n";
  OutputDebugStringA(debugger_message.c_str());
#endif
}

bool ShouldLogMismatch(std::uint64_t count) {
  if (count <= 8) return true;
  return count != 0 && (count & (count - 1)) == 0;
}

#if DESKLINK_ENABLE_RUST_CORE_SHADOW
static_assert(kStatusOk == DESKLINK_CORE_STATUS_OK);
static_assert(kStatusStale == DESKLINK_CORE_STATUS_STALE_EVENT);
static_assert(kEventStart == DESKLINK_CORE_EVENT_START);
static_assert(kEventClosed == DESKLINK_CORE_EVENT_CLOSED);
static_assert(kCommandBeginSignaling == DESKLINK_CORE_COMMAND_BEGIN_SIGNALING);
static_assert(kCommandSessionClosed == DESKLINK_CORE_COMMAND_SESSION_CLOSED);
#endif

}  // namespace

RustCoreShadow::RustCoreShadow() {
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  DeskLinkCoreHandle* created = nullptr;
  const std::int32_t status = desklink_core_create(&created);
  if (status != DESKLINK_CORE_STATUS_OK || created == nullptr) {
    std::ostringstream message;
    message << "create returned status=" << status;
    LogShadowMismatch(message.str());
    return;
  }
  handle_ = created;
#endif
}

RustCoreShadow::~RustCoreShadow() {
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  std::scoped_lock lock(mutex_);
  if (handle_ != nullptr) {
    const std::int32_t status =
        desklink_core_destroy(static_cast<DeskLinkCoreHandle*>(handle_));
    if (status != DESKLINK_CORE_STATUS_OK) {
      std::ostringstream message;
      message << "destroy returned status=" << status;
      LogShadowMismatch(message.str());
    }
    handle_ = nullptr;
  }
#endif
}

bool RustCoreShadow::available() const noexcept {
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  return handle_ != nullptr;
#else
  return false;
#endif
}

std::uint64_t RustCoreShadow::mismatch_count() const {
  std::scoped_lock lock(mutex_);
  return mismatch_count_;
}

bool RustCoreShadow::Reset() {
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  std::scoped_lock lock(mutex_);
  bool ok = true;
  if (handle_ != nullptr) {
    const std::int32_t destroy_status =
        desklink_core_destroy(static_cast<DeskLinkCoreHandle*>(handle_));
    if (destroy_status != DESKLINK_CORE_STATUS_OK) {
      std::ostringstream message;
      message << "reset destroy returned status=" << destroy_status;
      LogShadowMismatch(message.str());
      ok = false;
    }
    handle_ = nullptr;
  }

  DeskLinkCoreHandle* created = nullptr;
  const std::int32_t create_status = desklink_core_create(&created);
  if (create_status != DESKLINK_CORE_STATUS_OK || created == nullptr) {
    std::ostringstream message;
    message << "reset create returned status=" << create_status;
    LogShadowMismatch(message.str());
    return false;
  }
  handle_ = created;
  return ok;
#else
  return true;
#endif
}

bool RustCoreShadow::Observe(
    std::uint32_t kind,
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t control,
    std::uint64_t pointer,
    std::uint64_t operation,
    std::int32_t expected_status,
    std::uint32_t expected_command) {
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  std::unique_lock lock(mutex_);
  if (handle_ == nullptr) return true;

  DeskLinkCoreEvent event{};
  event.abi_version = DESKLINK_CORE_ABI_VERSION;
  event.kind = kind;
  event.session_generation = session;
  event.peer_generation = peer;
  event.control_generation = control;
  event.pointer_generation = pointer;
  event.operation_generation = operation;

  DeskLinkCoreCommandBuffer commands{};
  const std::int32_t status = desklink_core_apply(
      static_cast<DeskLinkCoreHandle*>(handle_),
      &event,
      &commands);

  bool command_matches = false;
  if (expected_command == kCommandNone) {
    command_matches = commands.len == 0;
  } else {
    command_matches = commands.len == 1 && commands.commands[0] == expected_command;
  }

  if (status == expected_status && command_matches) return true;

  ++mismatch_count_;
  const std::uint64_t mismatch_count = mismatch_count_;
  const bool should_log = ShouldLogMismatch(mismatch_count);
  std::string mismatch_message;
  if (should_log) {
    std::ostringstream message;
    message << "count=" << mismatch_count << " event=" << kind << " session=" << session
            << " peer=" << peer << " expected_status=" << expected_status
            << " actual_status=" << status << " expected_command=" << expected_command
            << " actual_len=" << commands.len;
    if (commands.len > 0) message << " actual_command=" << commands.commands[0];
    mismatch_message = message.str();
  }
  lock.unlock();

  if (should_log) LogShadowMismatch(mismatch_message);
  return false;
#else
  (void)kind;
  (void)session;
  (void)peer;
  (void)control;
  (void)pointer;
  (void)operation;
  (void)expected_status;
  (void)expected_command;
  return true;
#endif
}

bool RustCoreShadow::ObserveStart(std::uint64_t session) {
  return Observe(
      kEventStart,
      session,
      0,
      0,
      0,
      0,
      kStatusOk,
      kCommandBeginSignaling);
}

bool RustCoreShadow::ObserveStaleStart(std::uint64_t session) {
  return Observe(kEventStart, session, 0, 0, 0, 0, kStatusStale, kCommandNone);
}

bool RustCoreShadow::ObserveSignalConnected(std::uint64_t session) {
  return Observe(
      kEventSignalConnected,
      session,
      0,
      0,
      0,
      0,
      kStatusOk,
      kCommandBeginAuthentication);
}

bool RustCoreShadow::ObserveAuthenticationAccepted(
    std::uint64_t session,
    std::uint64_t peer) {
  return Observe(
      kEventAuthenticationAccepted,
      session,
      peer,
      0,
      0,
      0,
      kStatusOk,
      kCommandBeginNegotiation);
}

bool RustCoreShadow::ObservePeerConnected(std::uint64_t session, std::uint64_t peer) {
  return Observe(
      kEventPeerConnected,
      session,
      peer,
      0,
      0,
      0,
      kStatusOk,
      kCommandSessionConnected);
}

bool RustCoreShadow::ObserveStalePeerConnected(
    std::uint64_t session,
    std::uint64_t peer) {
  return Observe(
      kEventPeerConnected,
      session,
      peer,
      0,
      0,
      0,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObservePeerReplaced(std::uint64_t session, std::uint64_t peer) {
  return Observe(
      kEventPeerReplaced,
      session,
      peer,
      0,
      0,
      0,
      kStatusOk,
      kCommandBeginNegotiation);
}

bool RustCoreShadow::ObserveControlOpened(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t control) {
  return Observe(
      kEventControlOpened,
      session,
      peer,
      control,
      0,
      0,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObserveControlClosed(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t control) {
  return Observe(
      kEventControlClosed,
      session,
      peer,
      control,
      0,
      0,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObserveStaleControlOpened(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t control) {
  return Observe(
      kEventControlOpened,
      session,
      peer,
      control,
      0,
      0,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObserveStaleControlClosed(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t control) {
  return Observe(
      kEventControlClosed,
      session,
      peer,
      control,
      0,
      0,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObservePointerOpened(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t pointer) {
  return Observe(
      kEventPointerOpened,
      session,
      peer,
      0,
      pointer,
      0,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObservePointerClosed(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t pointer) {
  return Observe(
      kEventPointerClosed,
      session,
      peer,
      0,
      pointer,
      0,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObserveStalePointerOpened(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t pointer) {
  return Observe(
      kEventPointerOpened,
      session,
      peer,
      0,
      pointer,
      0,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObserveStalePointerClosed(
    std::uint64_t session,
    std::uint64_t peer,
    std::uint64_t pointer) {
  return Observe(
      kEventPointerClosed,
      session,
      peer,
      0,
      pointer,
      0,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObserveOperationStarted(
    std::uint64_t session,
    std::uint64_t operation) {
  return Observe(
      kEventOperationStarted,
      session,
      0,
      0,
      0,
      operation,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObserveOperationTimedOut(
    std::uint64_t session,
    std::uint64_t operation) {
  return Observe(
      kEventOperationTimedOut,
      session,
      0,
      0,
      0,
      operation,
      kStatusOk,
      kCommandNone);
}

bool RustCoreShadow::ObserveStaleOperationStarted(
    std::uint64_t session,
    std::uint64_t operation) {
  return Observe(
      kEventOperationStarted,
      session,
      0,
      0,
      0,
      operation,
      kStatusStale,
      kCommandNone);
}

bool RustCoreShadow::ObserveCloseRequested(std::uint64_t session) {
  return Observe(
      kEventCloseRequested,
      session,
      0,
      0,
      0,
      0,
      kStatusOk,
      kCommandBeginClose);
}

bool RustCoreShadow::ObserveClosed(std::uint64_t session) {
  return Observe(
      kEventClosed,
      session,
      0,
      0,
      0,
      0,
      kStatusOk,
      kCommandSessionClosed);
}

bool RustCoreShadowLifecycle::available() const noexcept {
  return shadow_.available();
}

std::uint64_t RustCoreShadowLifecycle::mismatch_count() const {
  return shadow_.mismatch_count();
}

std::uint64_t RustCoreShadowLifecycle::AdvanceNonZero(std::uint64_t* sequence) {
  if (sequence == nullptr) return 0;
  ++(*sequence);
  if (*sequence == 0) ++(*sequence);
  return *sequence;
}

bool RustCoreShadowLifecycle::EndSessionLocked() {
  if (!session_active_) return true;

  bool matches = true;
  if (peer_connected_) {
    matches = shadow_.ObserveCloseRequested(current_scope_.session) && matches;
    matches = shadow_.ObserveClosed(current_scope_.session) && matches;
  } else {
    // The Phase-1 model has no abort transition from Negotiating. Reset only the
    // observer after C++ has already abandoned that peer; do not invent a C++
    // production transition merely to satisfy the model.
    matches = shadow_.Reset() && matches;
  }

  session_active_ = false;
  peer_connected_ = false;
  current_scope_ = {};
  return matches;
}

RustCoreShadowPeerScope RustCoreShadowLifecycle::BeginPeer(
    bool same_authoritative_session) {
  std::scoped_lock lock(mutex_);

  if (session_active_ && !same_authoritative_session) {
    (void)EndSessionLocked();
  }

  if (!session_active_) {
    current_scope_.session = AdvanceNonZero(&session_sequence_);
    current_scope_.peer = AdvanceNonZero(&peer_sequence_);
    session_active_ = true;
    peer_connected_ = false;

    (void)shadow_.ObserveStart(current_scope_.session);
    (void)shadow_.ObserveSignalConnected(current_scope_.session);
    (void)shadow_.ObserveAuthenticationAccepted(
        current_scope_.session,
        current_scope_.peer);
    return current_scope_;
  }

  current_scope_.peer = AdvanceNonZero(&peer_sequence_);
  peer_connected_ = false;
  (void)shadow_.ObservePeerReplaced(
      current_scope_.session,
      current_scope_.peer);
  return current_scope_;
}

bool RustCoreShadowLifecycle::ComparePeerConnected(
    RustCoreShadowPeerScope scope,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative && scope == current_scope_) {
    peer_connected_ = true;
  }
  if (cpp_authoritative) {
    return shadow_.ObservePeerConnected(scope.session, scope.peer);
  }
  return shadow_.ObserveStalePeerConnected(scope.session, scope.peer);
}

bool RustCoreShadowLifecycle::CompareControlOpened(
    RustCoreShadowPeerScope scope,
    std::uint64_t control_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative) {
    return shadow_.ObserveControlOpened(
        scope.session,
        scope.peer,
        control_generation);
  }
  return shadow_.ObserveStaleControlOpened(
      scope.session,
      scope.peer,
      control_generation);
}

bool RustCoreShadowLifecycle::CompareControlClosed(
    RustCoreShadowPeerScope scope,
    std::uint64_t control_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative) {
    return shadow_.ObserveControlClosed(
        scope.session,
        scope.peer,
        control_generation);
  }
  return shadow_.ObserveStaleControlClosed(
      scope.session,
      scope.peer,
      control_generation);
}

bool RustCoreShadowLifecycle::ComparePointerOpened(
    RustCoreShadowPeerScope scope,
    std::uint64_t pointer_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative) {
    return shadow_.ObservePointerOpened(
        scope.session,
        scope.peer,
        pointer_generation);
  }
  return shadow_.ObserveStalePointerOpened(
      scope.session,
      scope.peer,
      pointer_generation);
}

bool RustCoreShadowLifecycle::ComparePointerClosed(
    RustCoreShadowPeerScope scope,
    std::uint64_t pointer_generation,
    bool cpp_authoritative) {
  std::scoped_lock lock(mutex_);
  if (cpp_authoritative) {
    return shadow_.ObservePointerClosed(
        scope.session,
        scope.peer,
        pointer_generation);
  }
  return shadow_.ObserveStalePointerClosed(
      scope.session,
      scope.peer,
      pointer_generation);
}

bool RustCoreShadowLifecycle::EndSession() {
  std::scoped_lock lock(mutex_);
  return EndSessionLocked();
}

}  // namespace desklink
