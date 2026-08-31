use criterion::{black_box, criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use desklink_protocol::{
    ControlChannelGeneration, PeerGeneration, PointerChannelGeneration, SessionGeneration,
};
use desklink_session::{RemoteSessionStateMachine, SessionEvent};

const EVENT_COUNTS: [usize; 2] = [100_000, 1_000_000];

fn apply(machine: &mut RemoteSessionStateMachine, event: SessionEvent) {
    black_box(machine.apply(event).expect("benchmark transition"));
}

fn connected_machine() -> (RemoteSessionStateMachine, SessionGeneration, PeerGeneration) {
    let session = SessionGeneration::initial();
    let peer = PeerGeneration::initial();
    let mut machine = RemoteSessionStateMachine::new();
    apply(&mut machine, SessionEvent::Start { session });
    apply(&mut machine, SessionEvent::SignalConnected { session });
    apply(
        &mut machine,
        SessionEvent::AuthenticationAccepted { session, peer },
    );
    apply(&mut machine, SessionEvent::PeerConnected { session, peer });
    (machine, session, peer)
}

fn run_valid_lifecycle(events: usize) {
    let mut machine = RemoteSessionStateMachine::new();
    let full_cycles = events / 6;
    let remainder = events % 6;

    for raw in 1..=full_cycles as u64 {
        let session = SessionGeneration::from_raw(raw).expect("nonzero session");
        let peer = PeerGeneration::from_raw(raw).expect("nonzero peer");
        apply(&mut machine, SessionEvent::Start { session });
        apply(&mut machine, SessionEvent::SignalConnected { session });
        apply(
            &mut machine,
            SessionEvent::AuthenticationAccepted { session, peer },
        );
        apply(&mut machine, SessionEvent::PeerConnected { session, peer });
        apply(&mut machine, SessionEvent::CloseRequested { session });
        apply(&mut machine, SessionEvent::Closed { session });
    }

    if remainder > 0 {
        let raw = full_cycles as u64 + 1;
        let session = SessionGeneration::from_raw(raw).expect("nonzero session");
        let peer = PeerGeneration::from_raw(raw).expect("nonzero peer");
        let tail = [
            SessionEvent::Start { session },
            SessionEvent::SignalConnected { session },
            SessionEvent::AuthenticationAccepted { session, peer },
            SessionEvent::PeerConnected { session, peer },
            SessionEvent::CloseRequested { session },
            SessionEvent::Closed { session },
        ];
        for event in tail.into_iter().take(remainder) {
            apply(&mut machine, event);
        }
    }

    black_box(machine.state());
}

fn run_stale_events(events: usize) {
    let (mut machine, session1, _) = connected_machine();
    apply(
        &mut machine,
        SessionEvent::CloseRequested { session: session1 },
    );
    apply(&mut machine, SessionEvent::Closed { session: session1 });
    let session2 = session1.next().expect("next session");
    apply(&mut machine, SessionEvent::Start { session: session2 });

    for _ in 0..events {
        apply(
            &mut machine,
            SessionEvent::SignalConnected { session: session1 },
        );
    }
    black_box(machine.current_session());
}

fn run_peer_replacements(events: usize) {
    let (mut machine, session, _) = connected_machine();
    for raw in 2..=(events / 2) as u64 + 1 {
        let peer = PeerGeneration::from_raw(raw).expect("nonzero peer");
        apply(&mut machine, SessionEvent::PeerReplaced { session, peer });
        apply(&mut machine, SessionEvent::PeerConnected { session, peer });
    }
    black_box(machine.current_peer());
}

fn run_control_replacements(events: usize) {
    let (mut machine, session, peer) = connected_machine();
    for raw in 1..=events as u64 {
        let control = ControlChannelGeneration::from_raw(raw).expect("nonzero control");
        apply(
            &mut machine,
            SessionEvent::ControlOpened {
                session,
                peer,
                control,
            },
        );
    }
    black_box(machine.current_control());
}

fn run_pointer_replacements(events: usize) {
    let (mut machine, session, peer) = connected_machine();
    for raw in 1..=events as u64 {
        let pointer = PointerChannelGeneration::from_raw(raw).expect("nonzero pointer");
        apply(
            &mut machine,
            SessionEvent::PointerOpened {
                session,
                peer,
                pointer,
            },
        );
    }
    black_box(machine.current_pointer());
}

fn session_benchmarks(c: &mut Criterion) {
    let mut group = c.benchmark_group("session_state_machine");
    group.sample_size(10);

    for events in EVENT_COUNTS {
        group.throughput(Throughput::Elements(events as u64));
        group.bench_with_input(
            BenchmarkId::new("valid_lifecycle", events),
            &events,
            |b, &n| {
                b.iter(|| run_valid_lifecycle(black_box(n)));
            },
        );
        group.bench_with_input(
            BenchmarkId::new("stale_events", events),
            &events,
            |b, &n| {
                b.iter(|| run_stale_events(black_box(n)));
            },
        );
        group.bench_with_input(
            BenchmarkId::new("peer_replacement", events),
            &events,
            |b, &n| {
                b.iter(|| run_peer_replacements(black_box(n)));
            },
        );
        group.bench_with_input(
            BenchmarkId::new("control_replacement", events),
            &events,
            |b, &n| {
                b.iter(|| run_control_replacements(black_box(n)));
            },
        );
        group.bench_with_input(
            BenchmarkId::new("pointer_replacement", events),
            &events,
            |b, &n| {
                b.iter(|| run_pointer_replacements(black_box(n)));
            },
        );
    }

    group.finish();
}

criterion_group!(benches, session_benchmarks);
criterion_main!(benches);
