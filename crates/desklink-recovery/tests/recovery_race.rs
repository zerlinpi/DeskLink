use desklink_protocol::SessionGeneration;
use desklink_recovery::{RecoveryCoordinator, RecoveryKind};

#[test]
fn recovery_storm_keeps_single_coordinator_flow() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);

    let first = coordinator.begin(RecoveryKind::Transport);
    let second = coordinator.begin(RecoveryKind::Transport);

    assert_eq!(first.kind, second.kind);
    assert_eq!(coordinator.current_session(), session);
}

#[test]
fn stale_recovery_cannot_change_new_session() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("next session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);

    let stale = coordinator.begin(RecoveryKind::Signaling);
    assert!(coordinator.rotate_session(session2));

    assert!(!coordinator.mark_recovered(session1, stale));
    assert_eq!(coordinator.current_session(), session2);
}
