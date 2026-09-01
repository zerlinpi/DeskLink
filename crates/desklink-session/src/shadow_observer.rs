//! Passive observer hooks for shadow-mode validation.
//!
//! Observers receive lifecycle information but cannot influence the
//! RemoteSessionStateMachine authority path.

use crate::SessionState;

use crate::SessionEvent;

pub trait SessionObserver {
    fn on_event(&mut self, event: &SessionEvent, state: SessionState);
}

#[derive(Debug, Default)]
pub struct NoopSessionObserver;

impl SessionObserver for NoopSessionObserver {
    fn on_event(&mut self, _event: &SessionEvent, _state: SessionState) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    struct CountingObserver {
        count: u32,
    }

    impl SessionObserver for CountingObserver {
        fn on_event(&mut self, _event: &SessionEvent, _state: SessionState) {
            self.count += 1;
        }
    }

    #[test]
    fn observer_is_passive() {
        let mut observer = CountingObserver { count: 0 };
        observer.on_event(&SessionEvent::CloseRequested { session: 1 }, SessionState::Idle);
        assert_eq!(observer.count, 1);
    }
}
