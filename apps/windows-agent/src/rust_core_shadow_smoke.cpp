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

  std::cout << "DeskLink Rust core shadow lifecycle + concurrency smoke passed.\n";
  return 0;
#endif
}
