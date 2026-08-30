#include "input_channel_authority.h"

#include <iostream>

int main() {
  desklink::InputChannelAuthority authority;

  const auto control1 = authority.Activate(desklink::InputChannelKind::Control);
  if (!authority.IsCurrent(desklink::InputChannelKind::Control, control1)) {
    std::cerr << "Initial control channel did not receive authority.\n";
    return 1;
  }

  const auto pointer1 = authority.Activate(desklink::InputChannelKind::Pointer);
  if (!authority.IsCurrent(desklink::InputChannelKind::Pointer, pointer1) ||
      !authority.IsCurrent(desklink::InputChannelKind::Control, control1)) {
    std::cerr << "Control and pointer authority must remain independent.\n";
    return 1;
  }

  const auto control2 = authority.Activate(desklink::InputChannelKind::Control);
  if (control2 == control1 ||
      authority.IsCurrent(desklink::InputChannelKind::Control, control1) ||
      !authority.IsCurrent(desklink::InputChannelKind::Control, control2)) {
    std::cerr << "Control replacement did not invalidate its predecessor.\n";
    return 1;
  }

  if (authority.RevokeIfCurrent(desklink::InputChannelKind::Control, control1) ||
      !authority.IsCurrent(desklink::InputChannelKind::Control, control2)) {
    std::cerr << "Stale control close revoked the active replacement.\n";
    return 1;
  }

  if (!authority.RevokeIfCurrent(desklink::InputChannelKind::Control, control2) ||
      authority.IsCurrent(desklink::InputChannelKind::Control, control2)) {
    std::cerr << "Current control close did not revoke authority.\n";
    return 1;
  }

  const auto control3 = authority.Activate(desklink::InputChannelKind::Control);
  authority.InvalidateAll();
  if (authority.IsCurrent(desklink::InputChannelKind::Control, control3) ||
      authority.IsCurrent(desklink::InputChannelKind::Pointer, pointer1)) {
    std::cerr << "Session invalidation left input authority active.\n";
    return 1;
  }

  const auto pointer2 = authority.Activate(desklink::InputChannelKind::Pointer);
  if (pointer2 == pointer1 ||
      !authority.IsCurrent(desklink::InputChannelKind::Pointer, pointer2)) {
    std::cerr << "Authority generations must not be reused after invalidation.\n";
    return 1;
  }

  std::cout << "DeskLink input channel authority smoke passed.\n";
  return 0;
}
