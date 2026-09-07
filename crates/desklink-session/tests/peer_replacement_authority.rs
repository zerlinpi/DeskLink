use desklink_protocol::{
    ControlChannelGeneration, PeerGeneration, PointerChannelGeneration, SessionGeneration,
};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent, SessionState};

fn connected_machine() -> (
    RemoteSessionStateMachine,
    SessionGeneration,
    PeerGeneration,
    ControlChannelGeneration,
) {
    let session = SessionGeneration::initial();
    let peer = PeerGeneration::initial();
    let control = ControlChannelGeneration::initial();
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
    machine
        .apply(SessionEvent::ControlOpened {
            session,
            peer,
            control,
        })
        .unwrap();

    (machine, session, peer, control)
}

fn connected_machine_with_channels() -> (
    RemoteSessionStateMachine,
    SessionGeneration,
    PeerGeneration,
    ControlChannelGeneration,
    PointerChannelGeneration,
) {
    let (mut machine, session, peer, control) = connected_machine();
    let pointer = PointerChannelGeneration::initial();

    machine
        .apply(SessionEvent::PointerOpened {
            session,
            peer,
            pointer,
        })
        .unwrap();

    (machine, session, peer, control, pointer)
}

#[test]
fn peer_replacement_revokes_old_control_authority_and_ignores_late_callback() {
    let (mut machine, session, peer1, control1) = connected_machine();
    let peer2 = peer1.next().expect("replacement peer generation");

    assert_eq!(machine.current_control(), Some(control1));
    assert_eq!(
        machine
            .apply(SessionEvent::PeerReplaced {
                session,
                peer: peer2,
            })
            .unwrap(),
        vec![SessionCommand::BeginNegotiation]
    );
    assert_eq!(machine.state(), SessionState::Negotiating);
    assert_eq!(machine.current_peer(), Some(peer2));
    assert_eq!(machine.current_control(), None);

    assert_eq!(
        machine
            .apply(SessionEvent::ControlOpened {
                session,
                peer: peer1,
                control: control1,
            })
            .unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(machine.state(), SessionState::Negotiating);
    assert_eq!(machine.current_peer(), Some(peer2));
    assert_eq!(machine.current_control(), None);
}

#[test]
fn stale_channel_callbacks_cannot_reclaim_or_poison_replacement_peer_authority() {
    let (mut machine, session, peer1, control1, pointer1) = connected_machine_with_channels();
    let peer2 = peer1.next().expect("replacement peer generation");
    let control2 = control1.next().expect("replacement control generation");
    let pointer2 = pointer1.next().expect("replacement pointer generation");

    assert_eq!(machine.current_control(), Some(control1));
    assert_eq!(machine.current_pointer(), Some(pointer1));

    assert_eq!(
        machine
            .apply(SessionEvent::PeerReplaced {
                session,
                peer: peer2,
            })
            .unwrap(),
        vec![SessionCommand::BeginNegotiation]
    );
    assert_eq!(machine.current_control(), None);
    assert_eq!(machine.current_pointer(), None);

    for event in [
        SessionEvent::ControlClosed {
            session,
            peer: peer1,
            control: control1,
        },
        SessionEvent::PointerClosed {
            session,
            peer: peer1,
            pointer: pointer1,
        },
        SessionEvent::ControlOpened {
            session,
            peer: peer1,
            control: control1,
        },
        SessionEvent::PointerOpened {
            session,
            peer: peer1,
            pointer: pointer1,
        },
    ] {
        assert_eq!(
            machine.apply(event).unwrap(),
            vec![SessionCommand::IgnoreStaleEvent]
        );
        assert_eq!(machine.current_control(), None);
        assert_eq!(machine.current_pointer(), None);
    }

    assert_eq!(
        machine
            .apply(SessionEvent::PeerConnected {
                session,
                peer: peer2,
            })
            .unwrap(),
        vec![SessionCommand::SessionConnected]
    );
    assert_eq!(machine.state(), SessionState::Connected);

    assert_eq!(
        machine
            .apply(SessionEvent::ControlOpened {
                session,
                peer: peer2,
                control: control2,
            })
            .unwrap(),
        Vec::new()
    );
    assert_eq!(
        machine
            .apply(SessionEvent::PointerOpened {
                session,
                peer: peer2,
                pointer: pointer2,
            })
            .unwrap(),
        Vec::new()
    );
    assert_eq!(machine.current_control(), Some(control2));
    assert_eq!(machine.current_pointer(), Some(pointer2));
}
