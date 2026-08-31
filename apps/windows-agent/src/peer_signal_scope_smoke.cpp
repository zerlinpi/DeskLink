#include "peer_signal_scope.h"

#include <iostream>

int main() {
  using desklink::PeerSignalScopeCurrent;

  if (!PeerSignalScopeCurrent(true, "controller-a", "controller-a", "session-a", "session-a")) {
    std::cerr << "current peer signaling scope was rejected\n";
    return 1;
  }
  if (PeerSignalScopeCurrent(false, "controller-a", "controller-a", "session-a", "session-a")) {
    std::cerr << "stale peer signaling scope was accepted\n";
    return 1;
  }
  if (PeerSignalScopeCurrent(true, "controller-b", "controller-a", "session-a", "session-a")) {
    std::cerr << "replaced controller signaling scope was accepted\n";
    return 1;
  }
  if (PeerSignalScopeCurrent(true, "controller-a", "controller-a", "session-b", "session-a")) {
    std::cerr << "replaced session signaling scope was accepted\n";
    return 1;
  }
  if (PeerSignalScopeCurrent(true, "", "", "session-a", "session-a") ||
      PeerSignalScopeCurrent(true, "controller-a", "controller-a", "", "")) {
    std::cerr << "empty signaling identity was accepted\n";
    return 1;
  }

  std::cout << "DeskLink peer signaling scope smoke passed.\n";
  return 0;
}
