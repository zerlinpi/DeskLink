//! Deterministic recovery scheduling with single-owner recovery leases.

mod lease;
mod levels;

use std::time::Duration;

use desklink_protocol::{OperationGeneration, SessionGeneration};

pub use lease::RecoveryLease;
pub use levels::RecoveryLevel;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RecoveryKind {
    Signaling,
    Transport,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RecoveryAttempt {
    pub operation: OperationGeneration,
    pub kind: RecoveryKind,
    pub delay: Duration,
}

#[derive(Debug)]
pub struct RecoveryCoordinator {
    session: SessionGeneration,
    next_operation: OperationGeneration,
    signaling_attempts: u32,
    transport_attempts: u32,
    active_signaling: Option<OperationGeneration>,
    active_transport: Option<OperationGeneration>,
    active_lease: Option<RecoveryLease>,
}

impl RecoveryCoordinator {
    pub const fn new(session: SessionGeneration) -> Self {
        Self {
            session,
            next_operation: OperationGeneration::initial(),
            signaling_attempts: 0,
            transport_attempts: 0,
            active_signaling: None,
            active_transport: None,
            active_lease: None,
        }
    }

    pub const fn current_session(&self) -> SessionGeneration {
        self.session
    }

    pub fn rotate_session(&mut self, session: SessionGeneration) -> bool {
        if session <= self.session {
            return false;
        }

        self.session = session;
        self.signaling_attempts = 0;
        self.transport_attempts = 0;
        self.active_signaling = None;
        self.active_transport = None;
        self.active_lease = None;
        true
    }

    pub fn active_lease(&self) -> Option<RecoveryLease> {
        self.active_lease
    }

    pub fn begin_with_level(&mut self, level: RecoveryLevel) -> Option<RecoveryAttempt> {
        if let Some(active) = self.active_lease {
            if !active.can_replace(level) {
                return None;
            }
        }

        let kind = match level {
            RecoveryLevel::SignalReconnect | RecoveryLevel::SessionRebuild => {
                RecoveryKind::Signaling
            }
            _ => RecoveryKind::Transport,
        };

        let attempt = self.begin_internal(kind);
        self.active_lease = Some(RecoveryLease::new(self.session, attempt.operation, level));
        Some(attempt)
    }

    fn begin_internal(&mut self, kind: RecoveryKind) -> RecoveryAttempt {
        let delay = match kind {
            RecoveryKind::Signaling => backoff_delay(self.signaling_attempts),
            RecoveryKind::Transport => backoff_delay(self.transport_attempts),
        };
        let operation = self.next_operation;
        self.next_operation = operation
            .next()
            .expect("recovery operation generation exhausted");

        match kind {
            RecoveryKind::Signaling => {
                self.signaling_attempts = self.signaling_attempts.saturating_add(1);
                self.active_signaling = Some(operation);
            }
            RecoveryKind::Transport => {
                self.transport_attempts = self.transport_attempts.saturating_add(1);
                self.active_transport = Some(operation);
            }
        }

        RecoveryAttempt {
            operation,
            kind,
            delay,
        }
    }

    pub fn mark_recovered(&mut self, session: SessionGeneration, attempt: RecoveryAttempt) -> bool {
        if session != self.session {
            return false;
        }

        let recovered = match attempt.kind {
            RecoveryKind::Signaling if self.active_signaling == Some(attempt.operation) => {
                self.signaling_attempts = 0;
                self.active_signaling = None;
                true
            }
            RecoveryKind::Transport if self.active_transport == Some(attempt.operation) => {
                self.transport_attempts = 0;
                self.active_transport = None;
                true
            }
            _ => false,
        };

        if recovered {
            self.active_lease = None;
        }

        recovered
    }
}

const fn backoff_delay(attempt: u32) -> Duration {
    let seconds = match attempt {
        0 => 0,
        1 => 1,
        2 => 2,
        3 => 4,
        4 => 8,
        _ => 15,
    };
    Duration::from_secs(seconds)
}
