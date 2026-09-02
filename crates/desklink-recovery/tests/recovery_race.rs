use desklink_protocol::SessionGeneration;
use desklink_recovery::{RecoveryCoordinator, RecoveryKind, RecoveryLevel};

#[test]
fn recovery_storm_keeps_single_coordinator_flow() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);

    let first = coordinator
        .begin_with_level(RecoveryLevel::DataChannelRebuild)
        .expect("start recovery");
    let second = coordinator.begin_with_level(RecoveryLevel::DataChannelRebuild);

    assert_eq!(first.kind, RecoveryKind::Transport);
    assert!(second.is_none());
    assert_eq!(
        coordinator.active_lease().expect("active recovery").operation,
        first.operation
    );
    assert_eq!(coordinator.current_session(), session);
}

#[test]
fn stale_recovery_cannot_change_new_session() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("next session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);

    let stale = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("start stale recovery");
    assert!(coordinator.rotate_session(session2));

    assert!(!coordinator.mark_recovered(session1, stale));
    assert_eq!(coordinator.current_session(), session2);
}

#[test]
fn escalation_revokes_the_previous_attempt() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);
    let transport = coordinator
        .begin_with_level(RecoveryLevel::IceRestart)
        .expect("start transport recovery");
    let signaling = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("escalate to signaling recovery");

    assert!(!coordinator.mark_failed(session, transport));
    assert!(coordinator.mark_failed(session, signaling));
}

#[test]
fn natural_recovery_does_not_take_recovery_authority() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);

    assert!(coordinator
        .begin_with_level(RecoveryLevel::Natural)
        .is_none());
    assert!(coordinator.active_lease().is_none());
}
