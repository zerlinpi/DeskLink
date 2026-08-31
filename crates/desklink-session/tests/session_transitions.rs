use desklink_protocol::{PeerGeneration, SessionGeneration};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent, SessionState};

#[test]
fn happy_path_transitions_emit_explicit_commands() {
    let session = SessionGeneration::initial();
    let peer = PeerGeneration::initial();
    let mut machine = RemoteSessionStateMachine::new();

    assert_eq!(machine.state(), SessionState::Idle);

    assert_eq!(
        machine.apply(SessionEvent::Start { session }).unwrap(),
        vec![SessionCommand::BeginSignaling]
    );
    assert_eq!(machine.state(), SessionState::Signaling);

    assert_eq!(
        machine
            .apply(SessionEvent::SignalConnected { session })
            .unwrap(),
        vec![SessionCommand::BeginAuthentication]
    );
    assert_eq!(machine.state(), SessionState::Authenticating);

    assert_eq!(
        machine
            .apply(SessionEvent::AuthenticationAccepted { session, peer })
            .unwrap(),
        vec![SessionCommand::BeginNegotiation]
    );
    assert_eq!(machine.state(), SessionState::Negotiating);

    assert_eq!(
        machine
            .apply(SessionEvent::PeerConnected { session, peer })
            .unwrap(),
        vec![SessionCommand::SessionConnected]
    );
    assert_eq!(machine.state(), SessionState::Connected);

    assert_eq!(
        machine
            .apply(SessionEvent::CloseRequested { session })
            .unwrap(),
        vec![SessionCommand::BeginClose]
    );
    assert_eq!(machine.state(), SessionState::Closing);

    assert_eq!(
        machine.apply(SessionEvent::Closed { session }).unwrap(),
        vec![SessionCommand::SessionClosed]
    );
    assert_eq!(machine.state(), SessionState::Idle);
}
