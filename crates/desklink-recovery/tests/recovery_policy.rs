use std::time::Duration;

use desklink_protocol::SessionGeneration;
use desklink_recovery::{RecoveryCoordinator, RecoveryKind, RecoveryLevel};

#[test]
fn signaling_backoff_is_bounded_and_resets_after_recovery() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);
    let expected = [0, 1, 2, 4, 8, 15, 15];
    let mut latest = None;

    for (index, seconds) in expected.iter().copied().enumerate() {
        let attempt = coordinator
            .begin_with_level(RecoveryLevel::SignalReconnect)
            .expect("start signaling recovery");
        assert_eq!(attempt.kind, RecoveryKind::Signaling);
        assert_eq!(attempt.delay, Duration::from_secs(seconds));
        if index + 1 != expected.len() {
            assert!(coordinator.mark_failed(session, attempt));
        }
        latest = Some(attempt);
    }

    assert!(coordinator.mark_recovered(session, latest.expect("latest signaling attempt")));
    assert_eq!(
        coordinator
            .begin_with_level(RecoveryLevel::SignalReconnect)
            .expect("retry signaling recovery")
            .delay,
        Duration::ZERO
    );
}

#[test]
fn stale_recovery_completion_cannot_reset_new_session_backoff() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("next session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);
    let stale_attempt = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("start stale recovery");

    assert!(coordinator.rotate_session(session2));
    let current_attempt = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("start current recovery");
    assert_eq!(current_attempt.delay, Duration::ZERO);

    assert!(!coordinator.mark_recovered(session1, stale_attempt));
    assert_eq!(coordinator.current_session(), session2);
    assert!(coordinator.mark_failed(session2, current_attempt));
    assert_eq!(
        coordinator
            .begin_with_level(RecoveryLevel::SignalReconnect)
            .expect("retry current recovery")
            .delay,
        Duration::from_secs(1)
    );
}
