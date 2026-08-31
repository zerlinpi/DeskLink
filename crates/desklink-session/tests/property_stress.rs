use desklink_protocol::{
    ControlChannelGeneration, OperationGeneration, PeerGeneration, PointerChannelGeneration,
    SessionGeneration,
};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent, SessionState};
use proptest::prelude::*;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Snapshot {
    state: SessionState,
    session: Option<SessionGeneration>,
    peer: Option<PeerGeneration>,
    control: Option<ControlChannelGeneration>,
    pointer: Option<PointerChannelGeneration>,
    operation: Option<OperationGeneration>,
}

fn snapshot(machine: &RemoteSessionStateMachine) -> Snapshot {
    Snapshot {
        state: machine.state(),
        session: machine.current_session(),
        peer: machine.current_peer(),
        control: machine.current_control(),
        pointer: machine.current_pointer(),
        operation: machine.current_operation(),
    }
}

fn connected_with(
    session: SessionGeneration,
    peer: PeerGeneration,
) -> RemoteSessionStateMachine {
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
}

fn assert_ignored_without_mutation(machine: &mut RemoteSessionStateMachine, event: SessionEvent) {
    let before = snapshot(machine);
    assert_eq!(
        machine.apply(event).unwrap(),
        vec![SessionCommand::IgnoreStaleEvent]
    );
    assert_eq!(snapshot(machine), before);
}

proptest! {
    #![proptest_config(ProptestConfig {
        cases: 10_000,
        ..ProptestConfig::default()
    })]

    #[test]
    fn stale_generations_never_replace_authority(
        session_raw in 2u64..1_000_000,
        peer_raw in 2u64..1_000_000,
        control_raw in 2u64..1_000_000,
        pointer_raw in 2u64..1_000_000,
        operation_raw in 2u64..1_000_000,
    ) {
        let session = SessionGeneration::from_raw(session_raw).unwrap();
        let stale_session = SessionGeneration::from_raw(session_raw - 1).unwrap();
        let peer = PeerGeneration::from_raw(peer_raw).unwrap();
        let stale_peer = PeerGeneration::from_raw(peer_raw - 1).unwrap();
        let control = ControlChannelGeneration::from_raw(control_raw).unwrap();
        let stale_control = ControlChannelGeneration::from_raw(control_raw - 1).unwrap();
        let pointer = PointerChannelGeneration::from_raw(pointer_raw).unwrap();
        let stale_pointer = PointerChannelGeneration::from_raw(pointer_raw - 1).unwrap();
        let operation = OperationGeneration::from_raw(operation_raw).unwrap();
        let stale_operation = OperationGeneration::from_raw(operation_raw - 1).unwrap();

        let mut machine = connected_with(session, peer);
        machine
            .apply(SessionEvent::ControlOpened {
                session,
                peer,
                control,
            })
            .unwrap();
        machine
            .apply(SessionEvent::PointerOpened {
                session,
                peer,
                pointer,
            })
            .unwrap();
        machine
            .apply(SessionEvent::OperationStarted { session, operation })
            .unwrap();

        let authoritative = snapshot(&machine);
        prop_assert_eq!(authoritative.state, SessionState::Connected);

        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::SignalConnected {
                session: stale_session,
            },
        );
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::PeerReplaced {
                session,
                peer: stale_peer,
            },
        );
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::ControlOpened {
                session,
                peer,
                control: stale_control,
            },
        );
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::PointerOpened {
                session,
                peer,
                pointer: stale_pointer,
            },
        );
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::OperationStarted {
                session,
                operation: stale_operation,
            },
        );
        prop_assert_eq!(snapshot(&machine), authoritative);

        machine
            .apply(SessionEvent::ControlClosed {
                session,
                peer,
                control,
            })
            .unwrap();
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::ControlOpened {
                session,
                peer,
                control: stale_control,
            },
        );
        prop_assert_eq!(machine.current_control(), None);

        machine
            .apply(SessionEvent::PointerClosed {
                session,
                peer,
                pointer,
            })
            .unwrap();
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::PointerOpened {
                session,
                peer,
                pointer: stale_pointer,
            },
        );
        prop_assert_eq!(machine.current_pointer(), None);

        machine
            .apply(SessionEvent::OperationTimedOut { session, operation })
            .unwrap();
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::OperationStarted {
                session,
                operation: stale_operation,
            },
        );
        prop_assert_eq!(machine.current_operation(), None);

        machine
            .apply(SessionEvent::CloseRequested { session })
            .unwrap();
        machine.apply(SessionEvent::Closed { session }).unwrap();
        assert_ignored_without_mutation(
            &mut machine,
            SessionEvent::Start {
                session: stale_session,
            },
        );
        prop_assert_eq!(machine.state(), SessionState::Idle);
        prop_assert_eq!(machine.current_session(), None);
    }
}

fn next_random(state: &mut u64) -> u64 {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    *state
}

#[test]
fn million_event_stale_race_stress_preserves_authority() {
    const EVENT_COUNT: usize = 1_000_000;

    let session = SessionGeneration::from_raw(100).unwrap();
    let stale_session = SessionGeneration::from_raw(99).unwrap();
    let peer = PeerGeneration::from_raw(100).unwrap();
    let stale_peer = PeerGeneration::from_raw(99).unwrap();
    let mut machine = connected_with(session, peer);

    let mut control_high_water = 2u64;
    let mut pointer_high_water = 2u64;
    let mut operation_high_water = 2u64;
    let mut control_current = Some(control_high_water);
    let mut pointer_current = Some(pointer_high_water);
    let mut operation_current = Some(operation_high_water);

    machine
        .apply(SessionEvent::ControlOpened {
            session,
            peer,
            control: ControlChannelGeneration::from_raw(control_high_water).unwrap(),
        })
        .unwrap();
    machine
        .apply(SessionEvent::PointerOpened {
            session,
            peer,
            pointer: PointerChannelGeneration::from_raw(pointer_high_water).unwrap(),
        })
        .unwrap();
    machine
        .apply(SessionEvent::OperationStarted {
            session,
            operation: OperationGeneration::from_raw(operation_high_water).unwrap(),
        })
        .unwrap();

    let mut random_state = 0xD35C_11A5_C0DE_2026u64;

    for _ in 0..EVENT_COUNT {
        match next_random(&mut random_state) % 11 {
            0 => {
                control_high_water += 1;
                control_current = Some(control_high_water);
                machine
                    .apply(SessionEvent::ControlOpened {
                        session,
                        peer,
                        control: ControlChannelGeneration::from_raw(control_high_water).unwrap(),
                    })
                    .unwrap();
            }
            1 => {
                assert_ignored_without_mutation(
                    &mut machine,
                    SessionEvent::ControlOpened {
                        session,
                        peer,
                        control: ControlChannelGeneration::from_raw(control_high_water - 1)
                            .unwrap(),
                    },
                );
            }
            2 => {
                if let Some(current) = control_current.take() {
                    machine
                        .apply(SessionEvent::ControlClosed {
                            session,
                            peer,
                            control: ControlChannelGeneration::from_raw(current).unwrap(),
                        })
                        .unwrap();
                } else {
                    assert_ignored_without_mutation(
                        &mut machine,
                        SessionEvent::ControlOpened {
                            session,
                            peer,
                            control: ControlChannelGeneration::from_raw(control_high_water).unwrap(),
                        },
                    );
                }
            }
            3 => {
                pointer_high_water += 1;
                pointer_current = Some(pointer_high_water);
                machine
                    .apply(SessionEvent::PointerOpened {
                        session,
                        peer,
                        pointer: PointerChannelGeneration::from_raw(pointer_high_water).unwrap(),
                    })
                    .unwrap();
            }
            4 => {
                assert_ignored_without_mutation(
                    &mut machine,
                    SessionEvent::PointerOpened {
                        session,
                        peer,
                        pointer: PointerChannelGeneration::from_raw(pointer_high_water - 1)
                            .unwrap(),
                    },
                );
            }
            5 => {
                if let Some(current) = pointer_current.take() {
                    machine
                        .apply(SessionEvent::PointerClosed {
                            session,
                            peer,
                            pointer: PointerChannelGeneration::from_raw(current).unwrap(),
                        })
                        .unwrap();
                } else {
                    assert_ignored_without_mutation(
                        &mut machine,
                        SessionEvent::PointerOpened {
                            session,
                            peer,
                            pointer: PointerChannelGeneration::from_raw(pointer_high_water).unwrap(),
                        },
                    );
                }
            }
            6 => {
                operation_high_water += 1;
                operation_current = Some(operation_high_water);
                machine
                    .apply(SessionEvent::OperationStarted {
                        session,
                        operation: OperationGeneration::from_raw(operation_high_water).unwrap(),
                    })
                    .unwrap();
            }
            7 => {
                assert_ignored_without_mutation(
                    &mut machine,
                    SessionEvent::OperationStarted {
                        session,
                        operation: OperationGeneration::from_raw(operation_high_water - 1)
                            .unwrap(),
                    },
                );
            }
            8 => {
                if let Some(current) = operation_current.take() {
                    machine
                        .apply(SessionEvent::OperationTimedOut {
                            session,
                            operation: OperationGeneration::from_raw(current).unwrap(),
                        })
                        .unwrap();
                } else {
                    assert_ignored_without_mutation(
                        &mut machine,
                        SessionEvent::OperationStarted {
                            session,
                            operation: OperationGeneration::from_raw(operation_high_water).unwrap(),
                        },
                    );
                }
            }
            9 => {
                assert_ignored_without_mutation(
                    &mut machine,
                    SessionEvent::SignalConnected {
                        session: stale_session,
                    },
                );
            }
            10 => {
                assert_ignored_without_mutation(
                    &mut machine,
                    SessionEvent::PeerReplaced {
                        session,
                        peer: stale_peer,
                    },
                );
            }
            _ => unreachable!(),
        }

        assert_eq!(machine.state(), SessionState::Connected);
        assert_eq!(machine.current_session(), Some(session));
        assert_eq!(machine.current_peer(), Some(peer));
        assert_eq!(
            machine.current_control().map(ControlChannelGeneration::get),
            control_current
        );
        assert_eq!(
            machine.current_pointer().map(PointerChannelGeneration::get),
            pointer_current
        );
        assert_eq!(
            machine.current_operation().map(OperationGeneration::get),
            operation_current
        );
    }
}
