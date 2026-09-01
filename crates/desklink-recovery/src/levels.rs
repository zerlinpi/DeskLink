//! Recovery escalation policy.
//!
//! This layer only describes recovery intent. It does not start network
//! operations directly. A single RecoveryCoordinator remains responsible for
//! ownership of recovery execution.

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum RecoveryLevel {
    Natural,
    DataChannelRebuild,
    IceRestart,
    PeerConnectionRecreate,
    SignalReconnect,
    SessionRebuild,
}

impl RecoveryLevel {
    pub const fn can_escalate_to(self, next: Self) -> bool {
        next >= self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recovery_escalation_is_monotonic() {
        assert!(RecoveryLevel::IceRestart.can_escalate_to(RecoveryLevel::SignalReconnect));
        assert!(!RecoveryLevel::SignalReconnect.can_escalate_to(RecoveryLevel::IceRestart));
    }
}
