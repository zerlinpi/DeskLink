//! Deterministic remote-session state machine prototype.

use desklink_protocol::{PeerGeneration, SessionGeneration};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionState {
    Idle,
    Signaling,
    Authenticating,
    Negotiating,
    Connected,
    Closing,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionEvent {
    Start {
        session: SessionGeneration,
    },
    SignalConnected {
        session: SessionGeneration,
    },
    AuthenticationAccepted {
        session: SessionGeneration,
        peer: PeerGeneration,
    },
    PeerConnected {
        session: SessionGeneration,
        peer: PeerGeneration,
    },
    CloseRequested {
        session: SessionGeneration,
    },
    Closed {
        session: SessionGeneration,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionCommand {
    BeginSignaling,
    BeginAuthentication,
    BeginNegotiation,
    SessionConnected,
    BeginClose,
    SessionClosed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionError {
    InvalidTransition { state: SessionState },
}

#[derive(Debug)]
pub struct RemoteSessionStateMachine {
    state: SessionState,
    session: Option<SessionGeneration>,
    peer: Option<PeerGeneration>,
}

impl Default for RemoteSessionStateMachine {
    fn default() -> Self {
        Self::new()
    }
}

impl RemoteSessionStateMachine {
    pub const fn new() -> Self {
        Self {
            state: SessionState::Idle,
            session: None,
            peer: None,
        }
    }

    pub const fn state(&self) -> SessionState {
        self.state
    }

    fn session_is_current(&self, session: SessionGeneration) -> bool {
        self.session == Some(session)
    }

    fn peer_is_current(&self, peer: PeerGeneration) -> bool {
        self.peer == Some(peer)
    }

    pub fn apply(&mut self, event: SessionEvent) -> Result<Vec<SessionCommand>, SessionError> {
        let command = match event {
            SessionEvent::Start { session } if self.state == SessionState::Idle => {
                self.session = Some(session);
                self.peer = None;
                self.state = SessionState::Signaling;
                SessionCommand::BeginSignaling
            }
            SessionEvent::SignalConnected { session }
                if self.state == SessionState::Signaling && self.session_is_current(session) =>
            {
                self.state = SessionState::Authenticating;
                SessionCommand::BeginAuthentication
            }
            SessionEvent::AuthenticationAccepted { session, peer }
                if self.state == SessionState::Authenticating
                    && self.session_is_current(session) =>
            {
                self.peer = Some(peer);
                self.state = SessionState::Negotiating;
                SessionCommand::BeginNegotiation
            }
            SessionEvent::PeerConnected { session, peer }
                if self.state == SessionState::Negotiating
                    && self.session_is_current(session)
                    && self.peer_is_current(peer) =>
            {
                self.state = SessionState::Connected;
                SessionCommand::SessionConnected
            }
            SessionEvent::CloseRequested { session }
                if self.state == SessionState::Connected && self.session_is_current(session) =>
            {
                self.state = SessionState::Closing;
                SessionCommand::BeginClose
            }
            SessionEvent::Closed { session }
                if self.state == SessionState::Closing && self.session_is_current(session) =>
            {
                self.session = None;
                self.peer = None;
                self.state = SessionState::Idle;
                SessionCommand::SessionClosed
            }
            _ => {
                return Err(SessionError::InvalidTransition { state: self.state });
            }
        };

        Ok(vec![command])
    }
}
