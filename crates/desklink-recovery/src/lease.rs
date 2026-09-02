//! Single-owner recovery lease.
//!
//! This module only models ownership. Execution remains inside
//! RecoveryCoordinator.

use desklink_protocol::{OperationGeneration, SessionGeneration};

use crate::levels::RecoveryLevel;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RecoveryLease {
    pub session: SessionGeneration,
    pub operation: OperationGeneration,
    pub level: RecoveryLevel,
}

impl RecoveryLease {
    pub const fn new(
        session: SessionGeneration,
        operation: OperationGeneration,
        level: RecoveryLevel,
    ) -> Self {
        Self {
            session,
            operation,
            level,
        }
    }

    pub fn can_replace(&self, next: RecoveryLevel) -> bool {
        next > self.level
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lease_only_allows_escalation() {
        let lease = RecoveryLease::new(
            SessionGeneration::initial(),
            OperationGeneration::initial(),
            RecoveryLevel::IceRestart,
        );

        assert!(lease.can_replace(RecoveryLevel::SignalReconnect));
        assert!(!lease.can_replace(RecoveryLevel::IceRestart));
        assert!(!lease.can_replace(RecoveryLevel::DataChannelRebuild));
    }
}
