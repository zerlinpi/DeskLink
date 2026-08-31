use desklink_protocol::{
    ControlChannelGeneration, OperationGeneration, PeerGeneration, PointerChannelGeneration,
    SessionGeneration,
};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent, SessionState};

fn connected_machine() -> (
    RemoteSessionStateMachine,
    SessionGeneration,
    PeerGeneration,
) {
    let session = SessionGeneration::initial();
    let peer = PeerGeneration::initial();
    let mut machine = RemoteSessionStateMachine::new();
    machine.apply(SessionEvent::Start { session }).unwrap();
    machine
        .apply(SessionEvent::SignalConnected { session })
        .unwrap();
    machine
        .apply(SessionEvent::AuthenticationAccepted { session, peer })
        .unwrap();
    machine
        .apply(SessionEvent::PeerConnected { session, peer })
        .unwrap();
    (machine, session, peer)
}

#[test]
fn old_session_event_is_ignored_after_session_rotation() {
    let (mut machine, session1, _peer1) = connected_machine();
    machine
        .apply(SessionEvent::CloseRequested { session: session1 })
        .unwrap();
    machine
        .apply(SessionEvent::Closed { session: session1 })
        .unwrap();

    let session2 = session1.next().expect("next session generation");
    machine
        .apply(SessionEvent::Start { session: session2 })
        .unwrap();

    assert_eq!(
        machine
            .apply(SessionEvent::SignalConnected { session: session1 })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.state(), SessionState::Signaling);
    assert_eq!(machine.current_session(), Some(session2));
}

#[test]
fn old_peer_connect_is_ignored_after_peer_replacement() {
    let (mut machine, session, peer1) = connected_machine();
    let peer2 = peer1.next().expect("next peer generation");

    machine
        .apply(SessionEvent::PeerReplaced {
            session,
            peer: peer2,
        })
        .unwrap();

    assert_eq!(
        machine
            .apply(SessionEvent::PeerConnected {
                session,
                peer: peer1,
            })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.state(), SessionState::Negotiating);
    assert_eq!(machine.current_peer(), Some(peer2));
}

#[test]
fn stale_control_close_does_not_revoke_replacement_channel() {
    let (mut machine, session, peer) = connected_machine();
    let control1 = ControlChannelGeneration::initial();
    let control2 = control1.next().expect("next control generation");

    machine
        .apply(SessionEvent::ControlOpened {
            session,
            peer,
            control: control1,
        })
        .unwrap();
    machine
        .apply(SessionEvent::ControlOpened {
            session,
            peer,
            control: control2,
        })
        .unwrap();

    assert_eq!(
        machine
            .apply(SessionEvent::ControlClosed {
                session,
                peer,
                control: control1,
            })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.current_control(), Some(control2));
}

#[test]
fn stale_pointer_close_does_not_revoke_replacement_channel() {
    let (mut machine, session, peer) = connected_machine();
    let pointer1 = PointerChannelGeneration::initial();
    let pointer2 = pointer1.next().expect("next pointer generation");

    machine
        .apply(SessionEvent::PointerOpened {
            session,
            peer,
            pointer: pointer1,
        })
        .unwrap();
    machine
        .apply(SessionEvent::PointerOpened {
            session,
            peer,
            pointer: pointer2,
        })
        .unwrap();

    assert_eq!(
        machine
            .apply(SessionEvent::PointerClosed {
                session,
                peer,
                pointer: pointer1,
            })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.current_pointer(), Some(pointer2));
}

#[test]
fn old_operation_timeout_is_ignored_after_new_operation_starts() {
    let (mut machine, session, _peer) = connected_machine();
    let operation1 = OperationGeneration::initial();
    let operation2 = operation1.next().expect("next operation generation");

    machine
        .apply(SessionEvent::OperationStarted {
            session,
            operation: operation1,
        })
        .unwrap();
    machine
        .apply(SessionEvent::OperationStarted {
            session,
            operation: operation2,
        })
        .unwrap();

    assert_eq!(
        machine
            .apply(SessionEvent::OperationTimedOut {
                session,
                operation: operation1,
            })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.current_operation(), Some(operation2));
}
