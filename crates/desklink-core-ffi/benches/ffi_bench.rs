use std::ptr::null_mut;

use criterion::{
    black_box, criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput,
};
use desklink_core_ffi::{
    desklink_core_apply, desklink_core_create, desklink_core_destroy, DeskLinkCoreCommandBuffer,
    DeskLinkCoreEvent, DeskLinkCoreHandle, DESKLINK_CORE_ABI_VERSION,
    DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED, DESKLINK_CORE_EVENT_OPERATION_STARTED,
    DESKLINK_CORE_EVENT_OPERATION_TIMED_OUT, DESKLINK_CORE_EVENT_PEER_CONNECTED,
    DESKLINK_CORE_EVENT_SIGNAL_CONNECTED, DESKLINK_CORE_EVENT_START, DESKLINK_CORE_STATUS_OK,
    DESKLINK_CORE_STATUS_STALE_EVENT,
};
use desklink_protocol::{OperationGeneration, PeerGeneration, SessionGeneration};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionEvent};

const EVENT_COUNTS: [usize; 2] = [100_000, 1_000_000];

fn direct_connected() -> (RemoteSessionStateMachine, SessionGeneration) {
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
    (machine, session)
}

fn direct_current_setup() -> (RemoteSessionStateMachine, SessionGeneration) {
    direct_connected()
}

fn direct_stale_setup() -> (
    RemoteSessionStateMachine,
    SessionGeneration,
    OperationGeneration,
) {
    let (mut machine, session) = direct_connected();
    let operation1 = OperationGeneration::initial();
    let operation2 = operation1.next().expect("next operation");
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
    (machine, session, operation1)
}

fn run_direct_current(mut setup: (RemoteSessionStateMachine, SessionGeneration), events: usize) {
    let (machine, session) = &mut setup;
    for raw in 1..=events as u64 {
        let operation = OperationGeneration::from_raw(raw).expect("nonzero operation");
        let result = machine
            .apply(SessionEvent::OperationStarted {
                session: *session,
                operation,
            })
            .expect("current direct event");
        black_box(result);
    }
}

fn run_direct_stale(
    mut setup: (
        RemoteSessionStateMachine,
        SessionGeneration,
        OperationGeneration,
    ),
    events: usize,
) {
    let (machine, session, stale_operation) = &mut setup;
    for _ in 0..events {
        let result = machine
            .apply(SessionEvent::OperationTimedOut {
                session: *session,
                operation: *stale_operation,
            })
            .expect("stale direct event");
        debug_assert_eq!(result, vec![SessionCommand::IgnoreStaleEvent]);
        black_box(result);
    }
}

struct CoreHandle(*mut DeskLinkCoreHandle);

impl CoreHandle {
    fn new() -> Self {
        let mut handle = null_mut();
        let status = unsafe { desklink_core_create(&mut handle) };
        assert_eq!(status, DESKLINK_CORE_STATUS_OK);
        assert!(!handle.is_null());
        Self(handle)
    }

    fn apply(&mut self, event: &DeskLinkCoreEvent) -> i32 {
        let mut commands = DeskLinkCoreCommandBuffer::default();
        let status = unsafe { desklink_core_apply(self.0, event, &mut commands) };
        black_box(commands);
        status
    }
}

impl Drop for CoreHandle {
    fn drop(&mut self) {
        let status = unsafe { desklink_core_destroy(self.0) };
        assert_eq!(status, DESKLINK_CORE_STATUS_OK);
        self.0 = null_mut();
    }
}

fn ffi_event(kind: u32) -> DeskLinkCoreEvent {
    DeskLinkCoreEvent {
        abi_version: DESKLINK_CORE_ABI_VERSION,
        kind,
        session_generation: 1,
        peer_generation: 0,
        control_generation: 0,
        pointer_generation: 0,
        operation_generation: 0,
    }
}

fn ffi_connected() -> CoreHandle {
    let mut core = CoreHandle::new();
    assert_eq!(
        core.apply(&ffi_event(DESKLINK_CORE_EVENT_START)),
        DESKLINK_CORE_STATUS_OK
    );
    assert_eq!(
        core.apply(&ffi_event(DESKLINK_CORE_EVENT_SIGNAL_CONNECTED)),
        DESKLINK_CORE_STATUS_OK
    );

    let mut authenticated = ffi_event(DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED);
    authenticated.peer_generation = 1;
    assert_eq!(core.apply(&authenticated), DESKLINK_CORE_STATUS_OK);

    let mut connected = ffi_event(DESKLINK_CORE_EVENT_PEER_CONNECTED);
    connected.peer_generation = 1;
    assert_eq!(core.apply(&connected), DESKLINK_CORE_STATUS_OK);
    core
}

fn ffi_current_setup() -> CoreHandle {
    ffi_connected()
}

fn ffi_stale_setup() -> (CoreHandle, DeskLinkCoreEvent) {
    let mut core = ffi_connected();
    let mut operation1 = ffi_event(DESKLINK_CORE_EVENT_OPERATION_STARTED);
    operation1.operation_generation = 1;
    let mut operation2 = operation1;
    operation2.operation_generation = 2;
    assert_eq!(core.apply(&operation1), DESKLINK_CORE_STATUS_OK);
    assert_eq!(core.apply(&operation2), DESKLINK_CORE_STATUS_OK);

    let mut stale_timeout = ffi_event(DESKLINK_CORE_EVENT_OPERATION_TIMED_OUT);
    stale_timeout.operation_generation = 1;
    (core, stale_timeout)
}

fn run_ffi_current(mut core: CoreHandle, events: usize) {
    for raw in 1..=events as u64 {
        let mut operation = ffi_event(DESKLINK_CORE_EVENT_OPERATION_STARTED);
        operation.operation_generation = raw;
        let status = core.apply(&operation);
        debug_assert_eq!(status, DESKLINK_CORE_STATUS_OK);
        black_box(status);
    }
}

fn run_ffi_stale(mut setup: (CoreHandle, DeskLinkCoreEvent), events: usize) {
    let (core, stale_timeout) = &mut setup;
    for _ in 0..events {
        let status = core.apply(stale_timeout);
        debug_assert_eq!(status, DESKLINK_CORE_STATUS_STALE_EVENT);
        black_box(status);
    }
}

fn ffi_benchmarks(c: &mut Criterion) {
    let mut group = c.benchmark_group("ffi_overhead");
    group.sample_size(10);

    for events in EVENT_COUNTS {
        group.throughput(Throughput::Elements(events as u64));

        group.bench_with_input(
            BenchmarkId::new("direct_current", events),
            &events,
            |b, &n| {
                b.iter_batched(
                    direct_current_setup,
                    |setup| run_direct_current(setup, black_box(n)),
                    BatchSize::SmallInput,
                );
            },
        );
        group.bench_with_input(BenchmarkId::new("ffi_current", events), &events, |b, &n| {
            b.iter_batched(
                ffi_current_setup,
                |setup| run_ffi_current(setup, black_box(n)),
                BatchSize::SmallInput,
            );
        });
        group.bench_with_input(
            BenchmarkId::new("direct_stale", events),
            &events,
            |b, &n| {
                b.iter_batched(
                    direct_stale_setup,
                    |setup| run_direct_stale(setup, black_box(n)),
                    BatchSize::SmallInput,
                );
            },
        );
        group.bench_with_input(BenchmarkId::new("ffi_stale", events), &events, |b, &n| {
            b.iter_batched(
                ffi_stale_setup,
                |setup| run_ffi_stale(setup, black_box(n)),
                BatchSize::SmallInput,
            );
        });
    }

    group.finish();
}

criterion_group!(benches, ffi_benchmarks);
criterion_main!(benches);
