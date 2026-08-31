use std::time::Duration;

use desklink_protocol::SessionGeneration;
use desklink_recovery::{RecoveryCoordinator, RecoveryKind};

#[test]
fn signaling_backoff_is_bounded_and_resets_after_recovery() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);
    let expected = [0, 1, 2, 4, 8, 15, 15];
    let mut latest = None;

    for seconds in expected {
        let attempt = coordinator.begin(RecoveryKind::Signaling);
        assert_eq!(attempt.kind, RecoveryKind::Signaling);
        assert_eq!(attempt.delay, Duration::from_secs(seconds));
        latest = Some(attempt);
    }

    assert!(coordinator.mark_recovered(session, latest.expect("latest signaling attempt")));
    assert_eq!(
        coordinator.begin(RecoveryKind::Signaling).delay,
        Duration::ZERO
    );
}

#[test]
fn stale_recovery_completion_cannot_reset_new_session_backoff() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("next session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);
    let stale_attempt = coordinator.begin(RecoveryKind::Signaling);

    assert!(coordinator.rotate_session(session2));
    let current_attempt = coordinator.begin(RecoveryKind::Signaling);
    assert_eq!(current_attempt.delay, Duration::ZERO);

    assert!(!coordinator.mark_recovered(session1, stale_attempt));
    assert_eq!(coordinator.current_session(), session2);
    assert_eq!(
        coordinator.begin(RecoveryKind::Signaling).delay,
        Duration::from_secs(1)
    );
}
