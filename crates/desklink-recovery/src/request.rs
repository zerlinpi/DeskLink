//! Unified recovery requests.
//!
//! Failure sources should describe intent here instead of directly
//! starting transport or signaling recovery.

use crate::levels::RecoveryLevel;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RecoveryReason {
    IceFailed,
    SignalLost,
    DataChannelClosed,
    PeerFailed,
    SessionTimeout,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RecoveryRequest {
    pub level: RecoveryLevel,
    pub reason: RecoveryReason,
}

impl RecoveryRequest {
    pub const fn new(level: RecoveryLevel, reason: RecoveryReason) -> Self {
        Self { level, reason }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_keeps_recovery_intent() {
        let request = RecoveryRequest::new(
            RecoveryLevel::IceRestart,
            RecoveryReason::IceFailed,
        );

        assert_eq!(request.level, RecoveryLevel::IceRestart);
        assert_eq!(request.reason, RecoveryReason::IceFailed);
    }
}
