#include "active_session.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "active session smoke failed: " << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  using desklink::InteractiveSessionCandidate;
  using desklink::SelectActiveInteractiveSession;
  using desklink::kInvalidInteractiveSession;

  Require(
      SelectActiveInteractiveSession(1, {
          InteractiveSessionCandidate{1, true, true},
          InteractiveSessionCandidate{7, true, true},
      }) == 1,
      "active physical console must win over RDP");

  Require(
      SelectActiveInteractiveSession(1, {
          InteractiveSessionCandidate{1, false, true},
          InteractiveSessionCandidate{7, true, true},
      }) == 7,
      "active RDP must be selected when console is not active");

  Require(
      SelectActiveInteractiveSession(kInvalidInteractiveSession, {
          InteractiveSessionCandidate{4, true, false},
          InteractiveSessionCandidate{7, true, true},
      }) == 7,
      "system sessions without a user must be ignored");

  Require(
      SelectActiveInteractiveSession(kInvalidInteractiveSession, {
          InteractiveSessionCandidate{9, true, true},
          InteractiveSessionCandidate{5, true, true},
      }) == 5,
      "multiple active non-console sessions must resolve deterministically");

  Require(
      SelectActiveInteractiveSession(1, {
          InteractiveSessionCandidate{1, false, true},
          InteractiveSessionCandidate{7, false, true},
      }) == kInvalidInteractiveSession,
      "no active interactive session must stay offline");

  std::cout << "Active interactive session selection smoke passed.\n";
  return 0;
}
