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
        coordinator
            .active_lease()
            .expect("active recovery")
            .operation,
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
fn repeated_session_rotation_keeps_latest_recovery_authority() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("second session generation");
    let session3 = session2.next().expect("third session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);

    let stale1 = coordinator
        .begin_with_level(RecoveryLevel::IceRestart)
        .expect("start first recovery");
    assert!(coordinator.rotate_session(session2));

    let stale2 = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("start second recovery");
    assert!(coordinator.rotate_session(session3));

    let current = coordinator
        .begin_with_level(RecoveryLevel::DataChannelRebuild)
        .expect("start current recovery");
    let current_lease = coordinator.active_lease().expect("current recovery lease");

    assert!(!coordinator.mark_recovered(session1, stale1));
    assert!(!coordinator.mark_failed(session2, stale2));
    assert_eq!(coordinator.current_session(), session3);
    assert_eq!(coordinator.active_lease(), Some(current_lease));

    assert!(coordinator.mark_recovered(session3, current));
    assert!(coordinator.active_lease().is_none());
}

#[test]
fn escalation_then_session_rotation_keeps_new_session_authority() {
    let session1 = SessionGeneration::initial();
    let session2 = session1.next().expect("next session generation");
    let mut coordinator = RecoveryCoordinator::new(session1);

    let transport = coordinator
        .begin_with_level(RecoveryLevel::IceRestart)
        .expect("start transport recovery");
    let escalated = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("escalate recovery");

    assert!(!coordinator.mark_recovered(session1, transport));
    assert!(coordinator.rotate_session(session2));

    let current = coordinator
        .begin_with_level(RecoveryLevel::DataChannelRebuild)
        .expect("start new session recovery");
    let current_lease = coordinator.active_lease().expect("new session lease");

    assert!(!coordinator.mark_failed(session1, transport));
    assert!(!coordinator.mark_recovered(session1, escalated));
    assert_eq!(coordinator.current_session(), session2);
    assert_eq!(coordinator.active_lease(), Some(current_lease));

    assert!(coordinator.mark_recovered(session2, current));
    assert!(coordinator.active_lease().is_none());
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
fn repeated_escalation_keeps_only_latest_recovery_authority() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);

    let transport = coordinator
        .begin_with_level(RecoveryLevel::IceRestart)
        .expect("start transport recovery");
    let signaling = coordinator
        .begin_with_level(RecoveryLevel::SignalReconnect)
        .expect("escalate to signaling recovery");
    let rebuild = coordinator
        .begin_with_level(RecoveryLevel::SessionRebuild)
        .expect("escalate to session rebuild");
    let current_lease = coordinator.active_lease().expect("session rebuild lease");

    assert_eq!(current_lease.operation, rebuild.operation);
    assert_eq!(current_lease.level, RecoveryLevel::SessionRebuild);
    assert!(!coordinator.mark_recovered(session, transport));
    assert!(!coordinator.mark_failed(session, signaling));
    assert_eq!(coordinator.active_lease(), Some(current_lease));

    assert!(coordinator.mark_recovered(session, rebuild));
    assert!(coordinator.active_lease().is_none());
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

#[test]
fn snapshot_exposes_recovery_state_without_taking_authority() {
    let session = SessionGeneration::initial();
    let mut coordinator = RecoveryCoordinator::new(session);
    let attempt = coordinator
        .begin_with_level(RecoveryLevel::IceRestart)
        .expect("start transport recovery");

    let snapshot = coordinator.snapshot();

    assert_eq!(snapshot.session, session);
    assert_eq!(snapshot.active_lease, coordinator.active_lease());
    assert_eq!(snapshot.active_lease.expect("active lease").operation, attempt.operation);
    assert_eq!(snapshot.transport_attempts, 1);
    assert_eq!(snapshot.signaling_attempts, 0);

    assert!(coordinator.mark_failed(session, attempt));
    let after_failure = coordinator.snapshot();
    assert!(after_failure.active_lease.is_none());
    assert_eq!(after_failure.transport_attempts, 1);
}
