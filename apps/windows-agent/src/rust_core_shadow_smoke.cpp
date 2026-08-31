#include "rust_core_shadow.h"

#ifndef DESKLINK_ENABLE_RUST_CORE_SHADOW
#define DESKLINK_ENABLE_RUST_CORE_SHADOW 0
#endif

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  desklink::RustCoreShadow shadow;
  Require(shadow.mismatch_count() == 0, "new shadow must start with zero mismatches");

#if !DESKLINK_ENABLE_RUST_CORE_SHADOW
  Require(!shadow.available(), "Rust shadow must stay unavailable when the CMake option is OFF");
  std::cout << "DeskLink Rust core shadow disabled-mode smoke passed.\n";
  return 0;
#else
  Require(shadow.available(), "Rust shadow core was not available");

  constexpr std::uint64_t kSession = 10;
  constexpr std::uint64_t kPeer = 20;
  constexpr std::uint64_t kControl = 30;
  constexpr std::uint64_t kPointer = 40;
  constexpr std::uint64_t kOperation = 50;

  Require(shadow.ObserveStart(kSession), "start decision mismatch");
  Require(shadow.ObserveSignalConnected(kSession), "signal decision mismatch");
  Require(
      shadow.ObserveAuthenticationAccepted(kSession, kPeer),
      "authentication decision mismatch");
  Require(shadow.ObservePeerConnected(kSession, kPeer), "peer decision mismatch");
  Require(
      shadow.ObserveControlOpened(kSession, kPeer, kControl),
      "control open decision mismatch");
  Require(
      shadow.ObservePointerOpened(kSession, kPeer, kPointer),
      "pointer open decision mismatch");
  Require(
      shadow.ObserveOperationStarted(kSession, kOperation),
      "operation start decision mismatch");

  Require(
      shadow.ObserveControlClosed(kSession, kPeer, kControl),
      "control close decision mismatch");
  Require(
      shadow.ObserveStaleControlOpened(kSession, kPeer, kControl - 1),
      "stale control generation was not rejected");

  Require(
      shadow.ObservePointerClosed(kSession, kPeer, kPointer),
      "pointer close decision mismatch");
  Require(
      shadow.ObserveStalePointerOpened(kSession, kPeer, kPointer - 1),
      "stale pointer generation was not rejected");

  Require(
      shadow.ObserveOperationTimedOut(kSession, kOperation),
      "operation timeout decision mismatch");
  Require(
      shadow.ObserveStaleOperationStarted(kSession, kOperation - 1),
      "stale operation generation was not rejected");

  Require(shadow.ObserveCloseRequested(kSession), "close request decision mismatch");
  Require(shadow.ObserveClosed(kSession), "closed decision mismatch");
  Require(
      shadow.ObserveStaleStart(kSession - 1),
      "closed session generation was resurrected");
  Require(shadow.mismatch_count() == 0, "matching lifecycle must not increment mismatch count");

  // A closed generation reported as authoritative is a deliberate C++/Rust mismatch.
  // Multiple callback threads may reach the observer in production, so the adapter must
  // serialize the single Rust handle and account every mismatch without losing updates.
  constexpr int kThreads = 8;
  constexpr int kMismatchesPerThread = 500;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&shadow]() {
      for (int i = 0; i < kMismatchesPerThread; ++i) {
        Require(
            !shadow.ObservePeerConnected(kSession, kPeer),
            "intentional stale peer mismatch was not reported");
      }
    });
  }
  for (auto& worker : workers) worker.join();
  Require(
      shadow.mismatch_count() == static_cast<std::uint64_t>(kThreads * kMismatchesPerThread),
      "concurrent shadow mismatch accounting lost observations");

  // The production-facing lifecycle adapter consumes C++ authority decisions instead of
  // inventing a second source of truth. It owns only internal Session/Peer generations;
  // Control/Pointer generations are supplied by the existing InputChannelAuthority.
  desklink::RustCoreShadowLifecycle lifecycle;
  Require(lifecycle.available(), "Rust lifecycle shadow was not available");

  const auto first = lifecycle.BeginPeer(false);
  Require(first.session != 0 && first.peer != 0, "first peer scope was invalid");
  Require(lifecycle.ComparePeerConnected(first, true), "first peer connected mismatch");
  Require(lifecycle.CompareControlOpened(first, 1, true), "first control open mismatch");
  Require(lifecycle.ComparePointerOpened(first, 1, true), "first pointer open mismatch");

  const auto replacement = lifecycle.BeginPeer(true);
  Require(replacement.session == first.session, "peer replacement rotated session generation");
  Require(replacement.peer > first.peer, "peer replacement generation did not advance");
  Require(
      lifecycle.ComparePeerConnected(first, false),
      "old peer connected callback was not classified stale");
  Require(
      lifecycle.CompareControlClosed(first, 1, false),
      "old peer control close was not classified stale");
  Require(
      lifecycle.ComparePointerClosed(first, 1, false),
      "old peer pointer close was not classified stale");
  Require(
      lifecycle.ComparePeerConnected(replacement, true),
      "replacement peer connected mismatch");
  Require(
      lifecycle.CompareControlOpened(replacement, 2, true),
      "replacement control open mismatch");
  Require(
      lifecycle.ComparePointerOpened(replacement, 2, true),
      "replacement pointer open mismatch");

  const auto next_session = lifecycle.BeginPeer(false);
  Require(next_session.session > replacement.session, "new authoritative session did not rotate generation");
  Require(next_session.peer > replacement.peer, "peer generation did not remain monotonic");
  Require(
      lifecycle.ComparePeerConnected(replacement, false),
      "old session peer callback was not classified stale");
  Require(
      lifecycle.ComparePeerConnected(next_session, true),
      "new session peer connected mismatch");
  Require(lifecycle.EndSession(), "connected shadow session did not close cleanly");
  Require(lifecycle.mismatch_count() == 0, "lifecycle adapter produced unexpected mismatches");

  // libdatachannel is free to deliver an input DataChannel open callback before
  // PeerConnection::Connected. C++ may already consider that channel current, but the
  // Rust reducer is still Negotiating. Cache the current channel authority and replay it
  // only after the peer-connected observation rather than reporting a false mismatch.
  desklink::RustCoreShadowEventBridge reordered;
  const auto early_scope = reordered.BeginPeer(false);
  Require(
      reordered.CompareControlOpened(early_scope, 11, true),
      "early authoritative control open produced a false mismatch");
  Require(
      reordered.ComparePointerOpened(early_scope, 12, true),
      "early authoritative pointer open produced a false mismatch");
  Require(
      reordered.ComparePeerConnected(early_scope, true),
      "peer connect did not replay early channel authority");
  Require(
      reordered.CompareControlClosed(early_scope, 11, true),
      "replayed control close mismatch");
  Require(
      reordered.ComparePointerClosed(early_scope, 12, true),
      "replayed pointer close mismatch");
  Require(reordered.EndSession(), "reordered shadow session did not close cleanly");
  Require(
      reordered.mismatch_count() == 0,
      "early channel callback ordering produced an unexpected mismatch");

  std::cout << "DeskLink Rust core shadow lifecycle + concurrency smoke passed.\n";
  return 0;
#endif
}
