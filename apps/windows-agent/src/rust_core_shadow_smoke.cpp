#include "rust_core_shadow.h"

#include <cstdlib>
#include <iostream>

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

  std::cout << "DeskLink Rust core shadow lifecycle smoke passed.\n";
  return 0;
}
