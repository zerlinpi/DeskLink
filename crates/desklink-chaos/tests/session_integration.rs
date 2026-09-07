use std::{cell::RefCell, rc::Rc};

use desklink_chaos::{
    ChaosEvaluation, ChaosEvaluator, ChaosOutcome, ChaosRunner, ChaosScenario, FaultInjector,
};
use desklink_protocol::{ControlChannelGeneration, PeerGeneration, SessionGeneration};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent, SessionState};

struct HarnessState {
    machine: RemoteSessionStateMachine,
    stale_events: u64,
}

struct SessionFaultInjector {
    state: Rc<RefCell<HarnessState>>,
    session: SessionGeneration,
    stale_peer: PeerGeneration,
    replacement_peer: PeerGeneration,
    stale_control: ControlChannelGeneration,
}

impl FaultInjector for SessionFaultInjector {
    fn inject(&mut self, scenario: &ChaosScenario) {
        assert_eq!(*scenario, ChaosScenario::PeerReplace);

        let mut state = self.state.borrow_mut();
        assert_eq!(
            state
                .machine
                .apply(SessionEvent::PeerReplaced {
                    session: self.session,
                    peer: self.replacement_peer,
                })
                .expect("replace peer"),
            vec![SessionCommand::BeginNegotiation]
        );

        let stale = state
            .machine
            .apply(SessionEvent::ControlOpened {
                session: self.session,
                peer: self.stale_peer,
                control: self.stale_control,
            })
            .expect("stale control callback is handled");
        if stale == vec![SessionCommand::IgnoreStaleEvent] {
            state.stale_events += 1;
        }
    }
}

struct SessionEvaluator {
    state: Rc<RefCell<HarnessState>>,
    replacement_peer: PeerGeneration,
}

impl ChaosEvaluator for SessionEvaluator {
    fn evaluate(&mut self, scenario: &ChaosScenario) -> ChaosEvaluation {
        if *scenario != ChaosScenario::PeerReplace {
            return ChaosEvaluation::not_executed();
        }

        let state = self.state.borrow();
        let recovered = state.machine.state() == SessionState::Negotiating
            && state.machine.current_peer() == Some(self.replacement_peer)
            && state.machine.current_control().is_none()
            && state.stale_events == 1;

        if recovered {
            ChaosEvaluation::recovered(Some("peer-replace".to_string()), state.stale_events, 1)
        } else {
            ChaosEvaluation::failed(Some("peer-replace".to_string()), state.stale_events, 1)
        }
    }
}

fn connected_state() -> (
    HarnessState,
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

    (
        HarnessState {
            machine,
            stale_events: 0,
        },
        session,
        peer,
        control,
    )
}

#[test]
fn peer_replace_chaos_rejects_stale_control_authority() {
    let (state, session, stale_peer, stale_control) = connected_state();
    let replacement_peer = stale_peer.next().expect("replacement peer generation");
    let state = Rc::new(RefCell::new(state));

    let injector = SessionFaultInjector {
        state: Rc::clone(&state),
        session,
        stale_peer,
        replacement_peer,
        stale_control,
    };
    let evaluator = SessionEvaluator {
        state: Rc::clone(&state),
        replacement_peer,
    };
    let mut runner = ChaosRunner::with_evaluator(injector, evaluator);

    let result = runner.run(ChaosScenario::PeerReplace);

    assert_eq!(result.outcome, ChaosOutcome::Recovered);
    assert_eq!(result.recovery_level.as_deref(), Some("peer-replace"));
    assert_eq!(result.stale_events, 1);
    assert_eq!(result.recovery_attempts, 1);
}
