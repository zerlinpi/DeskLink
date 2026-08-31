use std::mem::{align_of, size_of};
use std::ptr::null_mut;

use desklink_core_ffi::{
    desklink_core_apply, desklink_core_command_buffer_clear, desklink_core_create,
    desklink_core_destroy, desklink_core_test_force_panic, DeskLinkCoreCommandBuffer,
    DeskLinkCoreEvent, DeskLinkCoreHandle, DESKLINK_CORE_ABI_VERSION,
    DESKLINK_CORE_COMMAND_BEGIN_SIGNALING, DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED,
    DESKLINK_CORE_EVENT_CLOSED, DESKLINK_CORE_EVENT_CLOSE_REQUESTED,
    DESKLINK_CORE_EVENT_PEER_CONNECTED, DESKLINK_CORE_EVENT_SIGNAL_CONNECTED,
    DESKLINK_CORE_EVENT_START, DESKLINK_CORE_STATUS_INVALID_ARGUMENT, DESKLINK_CORE_STATUS_OK,
    DESKLINK_CORE_STATUS_PANIC, DESKLINK_CORE_STATUS_STALE_EVENT,
};

fn event(kind: u32, session: u64) -> DeskLinkCoreEvent {
    DeskLinkCoreEvent {
        abi_version: DESKLINK_CORE_ABI_VERSION,
        kind,
        session_generation: session,
        peer_generation: 0,
        control_generation: 0,
        pointer_generation: 0,
        operation_generation: 0,
    }
}

fn call_create(out_handle: *mut *mut DeskLinkCoreHandle) -> i32 {
    unsafe { desklink_core_create(out_handle) }
}

fn call_destroy(handle: *mut DeskLinkCoreHandle) -> i32 {
    unsafe { desklink_core_destroy(handle) }
}

fn call_apply(
    handle: *mut DeskLinkCoreHandle,
    event: *const DeskLinkCoreEvent,
    commands: *mut DeskLinkCoreCommandBuffer,
) -> i32 {
    unsafe { desklink_core_apply(handle, event, commands) }
}

fn call_clear(commands: *mut DeskLinkCoreCommandBuffer) -> i32 {
    unsafe { desklink_core_command_buffer_clear(commands) }
}

fn create_handle() -> *mut DeskLinkCoreHandle {
    let mut handle: *mut DeskLinkCoreHandle = null_mut();
    assert_eq!(call_create(&mut handle), DESKLINK_CORE_STATUS_OK);
    assert!(!handle.is_null());
    handle
}

fn apply_ok(
    handle: *mut DeskLinkCoreHandle,
    event: &DeskLinkCoreEvent,
) -> DeskLinkCoreCommandBuffer {
    let mut commands = DeskLinkCoreCommandBuffer::default();
    assert_eq!(
        call_apply(handle, event, &mut commands),
        DESKLINK_CORE_STATUS_OK
    );
    commands
}

#[test]
fn c_layout_is_fixed_width() {
    assert_eq!(size_of::<DeskLinkCoreEvent>(), 48);
    assert_eq!(align_of::<DeskLinkCoreEvent>(), 8);
    assert_eq!(size_of::<DeskLinkCoreCommandBuffer>(), 36);
    assert_eq!(align_of::<DeskLinkCoreCommandBuffer>(), 4);
}

#[test]
fn null_handles_and_null_outputs_are_rejected() {
    assert_eq!(
        call_create(null_mut()),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(
        call_destroy(null_mut()),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );

    let mut commands = DeskLinkCoreCommandBuffer::default();
    assert_eq!(
        call_apply(
            null_mut(),
            &event(DESKLINK_CORE_EVENT_START, 1),
            &mut commands
        ),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );

    let handle = create_handle();
    assert_eq!(
        call_apply(handle, std::ptr::null(), &mut commands),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(
        call_apply(handle, &event(DESKLINK_CORE_EVENT_START, 1), null_mut()),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
}

#[test]
fn unsupported_abi_version_is_rejected() {
    let handle = create_handle();
    let mut invalid = event(DESKLINK_CORE_EVENT_START, 1);
    invalid.abi_version = DESKLINK_CORE_ABI_VERSION + 1;
    let mut commands = DeskLinkCoreCommandBuffer::default();

    assert_eq!(
        call_apply(handle, &invalid, &mut commands),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
}

#[test]
fn invalid_event_kind_is_rejected() {
    let handle = create_handle();
    let mut commands = DeskLinkCoreCommandBuffer::default();

    assert_eq!(
        call_apply(handle, &event(u32::MAX, 1), &mut commands),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
}

#[test]
fn current_event_returns_ok_and_fixed_width_command() {
    let handle = create_handle();
    let commands = apply_ok(handle, &event(DESKLINK_CORE_EVENT_START, 1));

    assert_eq!(commands.len, 1);
    assert_eq!(commands.commands[0], DESKLINK_CORE_COMMAND_BEGIN_SIGNALING);
    assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
}

#[test]
fn stale_event_has_distinct_status_and_does_not_replace_current_session() {
    let handle = create_handle();

    apply_ok(handle, &event(DESKLINK_CORE_EVENT_START, 1));
    apply_ok(handle, &event(DESKLINK_CORE_EVENT_SIGNAL_CONNECTED, 1));

    let mut authenticated = event(DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED, 1);
    authenticated.peer_generation = 1;
    apply_ok(handle, &authenticated);

    let mut connected = event(DESKLINK_CORE_EVENT_PEER_CONNECTED, 1);
    connected.peer_generation = 1;
    apply_ok(handle, &connected);

    apply_ok(handle, &event(DESKLINK_CORE_EVENT_CLOSE_REQUESTED, 1));
    apply_ok(handle, &event(DESKLINK_CORE_EVENT_CLOSED, 1));
    apply_ok(handle, &event(DESKLINK_CORE_EVENT_START, 2));

    let mut commands = DeskLinkCoreCommandBuffer::default();
    assert_eq!(
        call_apply(
            handle,
            &event(DESKLINK_CORE_EVENT_SIGNAL_CONNECTED, 1),
            &mut commands,
        ),
        DESKLINK_CORE_STATUS_STALE_EVENT
    );
    assert_eq!(commands.len, 0);

    assert_eq!(
        call_apply(
            handle,
            &event(DESKLINK_CORE_EVENT_SIGNAL_CONNECTED, 2),
            &mut commands,
        ),
        DESKLINK_CORE_STATUS_OK
    );
    assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
}

#[test]
fn command_buffer_clear_zeroes_length_and_storage() {
    let mut commands = DeskLinkCoreCommandBuffer {
        len: 2,
        commands: [7, 9, 0, 0, 0, 0, 0, 0],
    };

    assert_eq!(call_clear(&mut commands), DESKLINK_CORE_STATUS_OK);
    assert_eq!(commands.len, 0);
    assert_eq!(commands.commands, [0; 8]);
    assert_eq!(
        call_clear(null_mut()),
        DESKLINK_CORE_STATUS_INVALID_ARGUMENT
    );
}

#[test]
fn ten_thousand_create_destroy_cycles_are_stable() {
    for _ in 0..10_000 {
        let handle = create_handle();
        assert_eq!(call_destroy(handle), DESKLINK_CORE_STATUS_OK);
    }
}

#[test]
fn forced_panic_is_caught_inside_the_ffi_boundary() {
    let result = std::panic::catch_unwind(desklink_core_test_force_panic);
    assert_eq!(result, Ok(DESKLINK_CORE_STATUS_PANIC));
}
