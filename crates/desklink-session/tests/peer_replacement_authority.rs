use desklink_protocol::{ControlChannelGeneration, PeerGeneration, SessionGeneration};
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
