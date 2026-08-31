//! Panic-safe C ABI prototype for the deterministic DeskLink core.

use std::panic::{catch_unwind, AssertUnwindSafe};

use desklink_protocol::{
    ControlChannelGeneration, OperationGeneration, PeerGeneration, PointerChannelGeneration,
    SessionGeneration,
};
use desklink_session::{RemoteSessionStateMachine, SessionCommand, SessionError, SessionEvent};

pub const DESKLINK_CORE_ABI_VERSION: u32 = 1;
pub const DESKLINK_CORE_COMMAND_CAPACITY: usize = 8;

pub const DESKLINK_CORE_STATUS_OK: i32 = 0;
pub const DESKLINK_CORE_STATUS_INVALID_ARGUMENT: i32 = 1;
pub const DESKLINK_CORE_STATUS_INVALID_TRANSITION: i32 = 2;
pub const DESKLINK_CORE_STATUS_STALE_EVENT: i32 = 3;
pub const DESKLINK_CORE_STATUS_PANIC: i32 = 4;

pub const DESKLINK_CORE_EVENT_START: u32 = 1;
pub const DESKLINK_CORE_EVENT_SIGNAL_CONNECTED: u32 = 2;
pub const DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED: u32 = 3;
pub const DESKLINK_CORE_EVENT_PEER_CONNECTED: u32 = 4;
pub const DESKLINK_CORE_EVENT_PEER_REPLACED: u32 = 5;
pub const DESKLINK_CORE_EVENT_CONTROL_OPENED: u32 = 6;
pub const DESKLINK_CORE_EVENT_CONTROL_CLOSED: u32 = 7;
pub const DESKLINK_CORE_EVENT_POINTER_OPENED: u32 = 8;
pub const DESKLINK_CORE_EVENT_POINTER_CLOSED: u32 = 9;
pub const DESKLINK_CORE_EVENT_OPERATION_STARTED: u32 = 10;
pub const DESKLINK_CORE_EVENT_OPERATION_TIMED_OUT: u32 = 11;
pub const DESKLINK_CORE_EVENT_CLOSE_REQUESTED: u32 = 12;
pub const DESKLINK_CORE_EVENT_CLOSED: u32 = 13;

pub const DESKLINK_CORE_COMMAND_BEGIN_SIGNALING: u32 = 1;
pub const DESKLINK_CORE_COMMAND_BEGIN_AUTHENTICATION: u32 = 2;
pub const DESKLINK_CORE_COMMAND_BEGIN_NEGOTIATION: u32 = 3;
pub const DESKLINK_CORE_COMMAND_SESSION_CONNECTED: u32 = 4;
pub const DESKLINK_CORE_COMMAND_BEGIN_CLOSE: u32 = 5;
pub const DESKLINK_CORE_COMMAND_SESSION_CLOSED: u32 = 6;

/// Opaque owner of the deterministic Rust session state machine.
pub struct DeskLinkCoreHandle {
    machine: RemoteSessionStateMachine,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeskLinkCoreEvent {
    pub abi_version: u32,
    pub kind: u32,
    pub session_generation: u64,
    pub peer_generation: u64,
    pub control_generation: u64,
    pub pointer_generation: u64,
    pub operation_generation: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeskLinkCoreCommandBuffer {
    pub len: u32,
    pub commands: [u32; DESKLINK_CORE_COMMAND_CAPACITY],
}

impl Default for DeskLinkCoreCommandBuffer {
    fn default() -> Self {
        Self {
            len: 0,
            commands: [0; DESKLINK_CORE_COMMAND_CAPACITY],
        }
    }
}

fn ffi_boundary(f: impl FnOnce() -> i32) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(status) => status,
        Err(_) => DESKLINK_CORE_STATUS_PANIC,
    }
}

fn clear_commands(commands: &mut DeskLinkCoreCommandBuffer) {
    commands.len = 0;
    commands.commands.fill(0);
}

fn decode_event(event: DeskLinkCoreEvent) -> Option<SessionEvent> {
    let session = SessionGeneration::from_raw(event.session_generation)?;

    match event.kind {
        DESKLINK_CORE_EVENT_START => Some(SessionEvent::Start { session }),
        DESKLINK_CORE_EVENT_SIGNAL_CONNECTED => Some(SessionEvent::SignalConnected { session }),
        DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            Some(SessionEvent::AuthenticationAccepted { session, peer })
        }
        DESKLINK_CORE_EVENT_PEER_CONNECTED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            Some(SessionEvent::PeerConnected { session, peer })
        }
        DESKLINK_CORE_EVENT_PEER_REPLACED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            Some(SessionEvent::PeerReplaced { session, peer })
        }
        DESKLINK_CORE_EVENT_CONTROL_OPENED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            let control = ControlChannelGeneration::from_raw(event.control_generation)?;
            Some(SessionEvent::ControlOpened {
                session,
                peer,
                control,
            })
        }
        DESKLINK_CORE_EVENT_CONTROL_CLOSED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            let control = ControlChannelGeneration::from_raw(event.control_generation)?;
            Some(SessionEvent::ControlClosed {
                session,
                peer,
                control,
            })
        }
        DESKLINK_CORE_EVENT_POINTER_OPENED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            let pointer = PointerChannelGeneration::from_raw(event.pointer_generation)?;
            Some(SessionEvent::PointerOpened {
                session,
                peer,
                pointer,
            })
        }
        DESKLINK_CORE_EVENT_POINTER_CLOSED => {
            let peer = PeerGeneration::from_raw(event.peer_generation)?;
            let pointer = PointerChannelGeneration::from_raw(event.pointer_generation)?;
            Some(SessionEvent::PointerClosed {
                session,
                peer,
                pointer,
            })
        }
        DESKLINK_CORE_EVENT_OPERATION_STARTED => {
            let operation = OperationGeneration::from_raw(event.operation_generation)?;
            Some(SessionEvent::OperationStarted { session, operation })
        }
        DESKLINK_CORE_EVENT_OPERATION_TIMED_OUT => {
            let operation = OperationGeneration::from_raw(event.operation_generation)?;
            Some(SessionEvent::OperationTimedOut { session, operation })
        }
        DESKLINK_CORE_EVENT_CLOSE_REQUESTED => Some(SessionEvent::CloseRequested { session }),
        DESKLINK_CORE_EVENT_CLOSED => Some(SessionEvent::Closed { session }),
        _ => None,
    }
}

fn encode_command(command: SessionCommand) -> Option<u32> {
    match command {
        SessionCommand::BeginSignaling => Some(DESKLINK_CORE_COMMAND_BEGIN_SIGNALING),
        SessionCommand::BeginAuthentication => Some(DESKLINK_CORE_COMMAND_BEGIN_AUTHENTICATION),
        SessionCommand::BeginNegotiation => Some(DESKLINK_CORE_COMMAND_BEGIN_NEGOTIATION),
        SessionCommand::SessionConnected => Some(DESKLINK_CORE_COMMAND_SESSION_CONNECTED),
        SessionCommand::BeginClose => Some(DESKLINK_CORE_COMMAND_BEGIN_CLOSE),
        SessionCommand::SessionClosed => Some(DESKLINK_CORE_COMMAND_SESSION_CLOSED),
        SessionCommand::IgnoreStaleEvent => None,
    }
}

fn write_commands(source: &[SessionCommand], target: &mut DeskLinkCoreCommandBuffer) -> i32 {
    clear_commands(target);

    if source == [SessionCommand::IgnoreStaleEvent] {
        return DESKLINK_CORE_STATUS_STALE_EVENT;
    }
    if source.len() > DESKLINK_CORE_COMMAND_CAPACITY {
        return DESKLINK_CORE_STATUS_PANIC;
    }

    for (index, command) in source.iter().copied().enumerate() {
        let Some(encoded) = encode_command(command) else {
            return DESKLINK_CORE_STATUS_PANIC;
        };
        target.commands[index] = encoded;
    }
    target.len = source.len() as u32;
    DESKLINK_CORE_STATUS_OK
}

/// Creates one opaque core handle and stores it in `out_handle`.
///
/// # Safety
/// `out_handle` must be either null or valid for writing one pointer. A successful call transfers
/// ownership of the returned handle to the caller, which must later pass it exactly once to
/// [`desklink_core_destroy`].
#[no_mangle]
pub unsafe extern "C" fn desklink_core_create(out_handle: *mut *mut DeskLinkCoreHandle) -> i32 {
    ffi_boundary(|| {
        if out_handle.is_null() {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        }

        let handle = Box::new(DeskLinkCoreHandle {
            machine: RemoteSessionStateMachine::new(),
        });
        unsafe {
            out_handle.write(Box::into_raw(handle));
        }
        DESKLINK_CORE_STATUS_OK
    })
}

/// Destroys a handle created by [`desklink_core_create`].
///
/// # Safety
/// `handle` must be null or a live pointer returned by [`desklink_core_create`] that has not been
/// destroyed before. Passing a dangling, foreign, or already-destroyed pointer is invalid.
#[no_mangle]
pub unsafe extern "C" fn desklink_core_destroy(handle: *mut DeskLinkCoreHandle) -> i32 {
    ffi_boundary(|| {
        if handle.is_null() {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        }

        unsafe {
            drop(Box::from_raw(handle));
        }
        DESKLINK_CORE_STATUS_OK
    })
}

/// Applies one fixed-width lifecycle event to the core state machine.
///
/// # Safety
/// `handle` must be a live DeskLink handle. `event` must point to a readable `DeskLinkCoreEvent`,
/// and `commands` must point to writable `DeskLinkCoreCommandBuffer` storage for the duration of
/// the call. These pointers must not alias in a way that violates Rust's exclusive access rules.
#[no_mangle]
pub unsafe extern "C" fn desklink_core_apply(
    handle: *mut DeskLinkCoreHandle,
    event: *const DeskLinkCoreEvent,
    commands: *mut DeskLinkCoreCommandBuffer,
) -> i32 {
    ffi_boundary(|| {
        if handle.is_null() || event.is_null() || commands.is_null() {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        }

        let event = unsafe { event.read() };
        let commands = unsafe { &mut *commands };
        clear_commands(commands);

        if event.abi_version != DESKLINK_CORE_ABI_VERSION {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        }
        let Some(event) = decode_event(event) else {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        };

        let handle = unsafe { &mut *handle };
        match handle.machine.apply(event) {
            Ok(produced) => write_commands(&produced, commands),
            Err(SessionError::InvalidTransition { .. }) => DESKLINK_CORE_STATUS_INVALID_TRANSITION,
        }
    })
}

/// Clears a fixed-width command buffer without allocating.
///
/// # Safety
/// `commands` must be null or point to writable `DeskLinkCoreCommandBuffer` storage.
#[no_mangle]
pub unsafe extern "C" fn desklink_core_command_buffer_clear(
    commands: *mut DeskLinkCoreCommandBuffer,
) -> i32 {
    ffi_boundary(|| {
        if commands.is_null() {
            return DESKLINK_CORE_STATUS_INVALID_ARGUMENT;
        }
        clear_commands(unsafe { &mut *commands });
        DESKLINK_CORE_STATUS_OK
    })
}

/// Prototype-only panic hook used by the ABI contract tests. It is intentionally absent from the
/// public C header.
#[doc(hidden)]
#[no_mangle]
pub extern "C" fn desklink_core_test_force_panic() -> i32 {
    ffi_boundary(|| panic!("forced DeskLink core FFI panic"))
}
