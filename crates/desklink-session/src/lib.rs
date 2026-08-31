//! Deterministic remote-session state machine prototype.

use std::cmp::Ordering;

use desklink_protocol::{
    ControlChannelGeneration, OperationGeneration, PeerGeneration, PointerChannelGeneration,
    SessionGeneration,
};

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
    PeerReplaced {
        session: SessionGeneration,
        peer: PeerGeneration,
    },
    ControlOpened {
        session: SessionGeneration,
        peer: PeerGeneration,
        control: ControlChannelGeneration,
    },
    ControlClosed {
        session: SessionGeneration,
        peer: PeerGeneration,
        control: ControlChannelGeneration,
    },
    PointerOpened {
        session: SessionGeneration,
        peer: PeerGeneration,
        pointer: PointerChannelGeneration,
    },
    PointerClosed {
        session: SessionGeneration,
        peer: PeerGeneration,
        pointer: PointerChannelGeneration,
    },
    OperationStarted {
        session: SessionGeneration,
        operation: OperationGeneration,
    },
    OperationTimedOut {
        session: SessionGeneration,
        operation: OperationGeneration,
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
    IgnoreStaleEvent,
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
    control: Option<ControlChannelGeneration>,
    pointer: Option<PointerChannelGeneration>,
    operation: Option<OperationGeneration>,
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
            control: None,
            pointer: None,
            operation: None,
        }
    }

    pub const fn state(&self) -> SessionState {
        self.state
    }

    pub const fn current_session(&self) -> Option<SessionGeneration> {
        self.session
    }

    pub const fn current_peer(&self) -> Option<PeerGeneration> {
        self.peer
    }

    pub const fn current_control(&self) -> Option<ControlChannelGeneration> {
        self.control
    }

    pub const fn current_pointer(&self) -> Option<PointerChannelGeneration> {
        self.pointer
    }

    pub const fn current_operation(&self) -> Option<OperationGeneration> {
        self.operation
    }

    fn session_ordering(&self, session: SessionGeneration) -> Option<Ordering> {
        self.session.map(|current| session.cmp(&current))
    }

    fn peer_ordering(&self, peer: PeerGeneration) -> Option<Ordering> {
        self.peer.map(|current| peer.cmp(&current))
    }

    fn control_ordering(&self, control: ControlChannelGeneration) -> Option<Ordering> {
        self.control.map(|current| control.cmp(&current))
    }

    fn pointer_ordering(&self, pointer: PointerChannelGeneration) -> Option<Ordering> {
        self.pointer.map(|current| pointer.cmp(&current))
    }

    fn operation_ordering(&self, operation: OperationGeneration) -> Option<Ordering> {
        self.operation.map(|current| operation.cmp(&current))
    }

    fn ignore_stale() -> Result<Vec<SessionCommand>, SessionError> {
        Ok(vec![SessionCommand::IgnoreStaleEvent])
    }

    fn invalid(&self) -> Result<Vec<SessionCommand>, SessionError> {
        Err(SessionError::InvalidTransition { state: self.state })
    }

    fn clear_scoped_authority(&mut self) {
        self.control = None;
        self.pointer = None;
        self.operation = None;
    }

    pub fn apply(&mut self, event: SessionEvent) -> Result<Vec<SessionCommand>, SessionError> {
        match event {
            SessionEvent::Start { session } if self.state == SessionState::Idle => {
                self.session = Some(session);
                self.peer = None;
                self.clear_scoped_authority();
                self.state = SessionState::Signaling;
                Ok(vec![SessionCommand::BeginSignaling])
            }
            SessionEvent::Start { .. } => self.invalid(),
            SessionEvent::SignalConnected { session } => match self.session_ordering(session) {
                Some(Ordering::Less) => Self::ignore_stale(),
                Some(Ordering::Equal) if self.state == SessionState::Signaling => {
                    self.state = SessionState::Authenticating;
                    Ok(vec![SessionCommand::BeginAuthentication])
                }
                _ => self.invalid(),
            },
            SessionEvent::AuthenticationAccepted { session, peer } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal) if self.state == SessionState::Authenticating => {
                        self.peer = Some(peer);
                        self.clear_scoped_authority();
                        self.state = SessionState::Negotiating;
                        Ok(vec![SessionCommand::BeginNegotiation])
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::PeerConnected { session, peer } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal) if self.state == SessionState::Negotiating => {
                        self.state = SessionState::Connected;
                        Ok(vec![SessionCommand::SessionConnected])
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::PeerReplaced { session, peer } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Greater)
                        if matches!(self.state, SessionState::Connected | SessionState::Negotiating) =>
                    {
                        self.peer = Some(peer);
                        self.clear_scoped_authority();
                        self.state = SessionState::Negotiating;
                        Ok(vec![SessionCommand::BeginNegotiation])
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::ControlOpened {
                session,
                peer,
                control,
            } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                if self.state != SessionState::Connected {
                    return self.invalid();
                }
                match self.control_ordering(control) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal | Ordering::Greater) | None => {
                        self.control = Some(control);
                        Ok(Vec::new())
                    }
                }
            }
            SessionEvent::ControlClosed {
                session,
                peer,
                control,
            } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.control_ordering(control) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal) => {
                        self.control = None;
                        Ok(Vec::new())
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::PointerOpened {
                session,
                peer,
                pointer,
            } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                if self.state != SessionState::Connected {
                    return self.invalid();
                }
                match self.pointer_ordering(pointer) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal | Ordering::Greater) | None => {
                        self.pointer = Some(pointer);
                        Ok(Vec::new())
                    }
                }
            }
            SessionEvent::PointerClosed {
                session,
                peer,
                pointer,
            } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.peer_ordering(peer) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.pointer_ordering(pointer) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal) => {
                        self.pointer = None;
                        Ok(Vec::new())
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::OperationStarted { session, operation } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                if self.state != SessionState::Connected {
                    return self.invalid();
                }
                match self.operation_ordering(operation) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal | Ordering::Greater) | None => {
                        self.operation = Some(operation);
                        Ok(Vec::new())
                    }
                }
            }
            SessionEvent::OperationTimedOut { session, operation } => {
                match self.session_ordering(session) {
                    Some(Ordering::Less) => return Self::ignore_stale(),
                    Some(Ordering::Equal) => {}
                    _ => return self.invalid(),
                }
                match self.operation_ordering(operation) {
                    Some(Ordering::Less) => Self::ignore_stale(),
                    Some(Ordering::Equal) => {
                        self.operation = None;
                        Ok(Vec::new())
                    }
                    _ => self.invalid(),
                }
            }
            SessionEvent::CloseRequested { session } => match self.session_ordering(session) {
                Some(Ordering::Less) => Self::ignore_stale(),
                Some(Ordering::Equal) if self.state == SessionState::Connected => {
                    self.clear_scoped_authority();
                    self.state = SessionState::Closing;
                    Ok(vec![SessionCommand::BeginClose])
                }
                _ => self.invalid(),
            },
            SessionEvent::Closed { session } => match self.session_ordering(session) {
                Some(Ordering::Less) => Self::ignore_stale(),
                Some(Ordering::Equal) if self.state == SessionState::Closing => {
                    self.session = None;
                    self.peer = None;
                    self.clear_scoped_authority();
                    self.state = SessionState::Idle;
                    Ok(vec![SessionCommand::SessionClosed])
                }
                _ => self.invalid(),
            },
        }
    }
}
