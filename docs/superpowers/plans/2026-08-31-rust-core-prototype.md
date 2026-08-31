# Rust Core Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a prototype-only Rust session/recovery core that can be benchmarked against DeskLink's current C++ coordination path without replacing production media, WebRTC, Signal, Service, or Web Controller paths.

**Architecture:** Add a small Rust workspace on `prototype/rust-core` with deterministic protocol/session/recovery crates and a narrow C ABI wrapper. The C++ Windows Agent remains authoritative during Phase 1; Rust is exercised by unit/property/stress tests and synthetic benchmarks first, then optionally by shadow-mode integration that mirrors lifecycle decisions without changing user-visible behavior.

**Tech Stack:** Rust stable, Cargo workspace, `serde`, `thiserror`, `proptest`, `criterion`, C ABI (`extern "C"`), existing C++20/Windows/libdatachannel production stack, GitHub Actions.

**Spec:** `docs/adr/ADR-001-rust-core.md`

## Global Constraints

- Production `main` remains unchanged functionally until benchmark and regression gates pass.
- Prototype work happens on `prototype/rust-core`.
- Do not replace DXGI, D3D11, Media Foundation, SendInput, libdatachannel/WebRTC, Windows Service architecture, Go Signal, or React Web Controller in Phase 1.
- RustDesk is architecture reference only; no AGPL source is copied.
- Every state-changing async event is scoped by explicit generation values; stale events fail closed.
- No Rust panic, C++ exception, or ambiguous allocation ownership may cross FFI.
- Existing C++/TypeScript scope/generation/recovery/auth/native-smoke tests remain the regression oracle.
- Rust authority is not enabled in production during Phase 1.

---

## Task 1: Isolated Rust workspace and prototype CI

**Files:**
- Create: `Cargo.toml`
- Create: `Cargo.lock`
- Create: `rust-toolchain.toml`
- Create: `crates/desklink-protocol/Cargo.toml`
- Create: `crates/desklink-protocol/src/lib.rs`
- Create: `.github/workflows/rust-core-prototype.yml`

**Interfaces:**
- Produces strongly typed `SessionGeneration`, `PeerGeneration`, `ControlChannelGeneration`, `PointerChannelGeneration`, `OperationGeneration`.

- [ ] Create `prototype/rust-core` from the commit containing ADR-001.
- [ ] Write a failing test that references `SessionGeneration::initial()/next()` and `PeerGeneration::initial()/next()` before the types exist.
- [ ] Run `cargo test -p desklink-protocol generations_are_strongly_typed_and_monotonic`; expect compile failure.
- [ ] Add the minimal workspace and generation newtypes, each wrapping `u64`, starting at 1 and using checked increment.
- [ ] Pin a stable Rust toolchain with `rustfmt` and `clippy` in `rust-toolchain.toml`.
- [ ] Add `rust-core-prototype.yml`, triggered only for `prototype/rust-core` pushes/PRs touching Rust prototype files. It must run `cargo fmt --check`, `cargo clippy --workspace --all-targets -- -D warnings`, and `cargo test --workspace --all-targets`; it must not request `contents: write` or push source.
- [ ] Run all three commands locally/CI and require PASS.
- [ ] Commit: `prototype(rust): add isolated core workspace`.

## Task 2: Deterministic RemoteSessionStateMachine

**Files:**
- Modify: `Cargo.toml`
- Modify: `crates/desklink-protocol/src/lib.rs`
- Create: `crates/desklink-session/Cargo.toml`
- Create: `crates/desklink-session/src/lib.rs`
- Create: `crates/desklink-session/tests/session_transitions.rs`
- Create: `crates/desklink-session/tests/stale_generation.rs`

**Interfaces:**
- Produces `SessionState`, `SessionEvent`, `SessionCommand`, `SessionError`, `RemoteSessionStateMachine::apply(SessionEvent) -> Result<Vec<SessionCommand>, SessionError>`.

- [ ] Write failing happy-path tests for `Idle -> Signaling -> Authenticating -> Negotiating -> Connected -> Closing -> Idle`; assert state and emitted command after every event.
- [ ] Run `cargo test -p desklink-session --test session_transitions`; expect failure because implementation does not exist.
- [ ] Implement only the explicit states/events/commands required by the tests. Impossible current-generation transitions return `SessionError::InvalidTransition`; they are never silently swallowed.
- [ ] Write failing stale tests for old session connect, old peer connect after replacement, stale control close, stale pointer close, and old operation timeout after a newer operation exists.
- [ ] Require stale events to return `SessionCommand::IgnoreStaleEvent` without mutating current state/generation.
- [ ] Add centralized `session_is_current`, `peer_is_current`, `control_is_current`, and `pointer_is_current` guards.
- [ ] Run `cargo test -p desklink-session` and workspace clippy; require PASS.
- [ ] Commit: `prototype(rust): add deterministic session state machine`.

## Task 3: RecoveryCoordinator without async-runtime coupling

**Files:**
- Modify: `Cargo.toml`
- Modify: `crates/desklink-protocol/src/lib.rs`
- Create: `crates/desklink-recovery/Cargo.toml`
- Create: `crates/desklink-recovery/src/lib.rs`
- Create: `crates/desklink-recovery/tests/recovery_policy.rs`

**Interfaces:**
- Produces `RecoveryKind::{Signaling,Transport}`, `RecoveryAttempt { operation, kind, delay }`, and deterministic `RecoveryCoordinator` methods.

- [ ] Write failing tests for signaling delays `0s, 1s, 2s, 4s, 8s, 15s, 15s` and reset after recovery.
- [ ] Write failing tests proving a recovery started under session generation N cannot mutate state after rotation to N+1.
- [ ] Run `cargo test -p desklink-recovery --test recovery_policy`; expect failure.
- [ ] Implement integer counters and fixed constants only; return scheduling decisions and let the caller own actual timers. Do not add Tokio in Phase 1.
- [ ] Run `cargo test -p desklink-recovery` and `cargo test --workspace`; require PASS.
- [ ] Commit: `prototype(rust): add recovery coordinator`.

## Task 4: Panic-safe C ABI

**Files:**
- Modify: `Cargo.toml`
- Create: `crates/desklink-core-ffi/Cargo.toml`
- Create: `crates/desklink-core-ffi/src/lib.rs`
- Create: `crates/desklink-core-ffi/include/desklink_core.h`
- Create: `crates/desklink-core-ffi/tests/ffi_contract.rs`

**Interfaces:**
- Opaque `DeskLinkCoreHandle`.
- Fixed-width `DeskLinkCoreEvent` with `abi_version`, event kind and generation fields.
- Stable status codes: OK, INVALID_ARGUMENT, INVALID_TRANSITION, STALE_EVENT, PANIC.
- `desklink_core_create`, `desklink_core_destroy`, `desklink_core_apply`, `desklink_core_command_buffer_clear`.

- [ ] Write failing ABI tests for null handles, unsupported ABI version, invalid event kind, current event, stale event, command-buffer clear, 10,000 create/destroy cycles, and a test-only forced panic.
- [ ] Run `cargo test -p desklink-core-ffi --test ffi_contract`; expect failure.
- [ ] Implement opaque ownership. No `Vec`, `String`, Rust reference, trait object, or Rust enum layout is exposed through C ABI.
- [ ] Wrap every exported entrypoint in `catch_unwind`; panics map to the stable PANIC status and never unwind into C++.
- [ ] Reject every `abi_version != 1`.
- [ ] Run `cargo test -p desklink-core-ffi`, workspace clippy, and `cargo build -p desklink-core-ffi --release`; require PASS.
- [ ] Commit: `prototype(rust): add panic-safe core ffi`.

## Task 5: Benchmark and regression gates

**Files:**
- Modify: `crates/desklink-session/Cargo.toml`
- Modify: `crates/desklink-core-ffi/Cargo.toml`
- Create: `crates/desklink-session/benches/session_bench.rs`
- Create: `crates/desklink-core-ffi/benches/ffi_bench.rs`
- Create: `tools/rust-core/benchmark-baseline.md`
- Create: `tools/rust-core/regression-matrix.md`

- [ ] Add Criterion only as a dev dependency.
- [ ] Benchmark 100k/1M valid lifecycle events, 100k/1M stale events, peer replacement and control/pointer replacement; record ns/event and throughput.
- [ ] Benchmark direct reducer calls versus `desklink_core_apply` for equivalent current/stale events to isolate FFI overhead.
- [ ] Run short benchmark smoke with `--warm-up-time 1 --measurement-time 2`; require numeric output but do not invent pass/fail thresholds before a baseline exists.
- [ ] In `benchmark-baseline.md`, define collection of Connection Success Rate, Time To First Frame, Input Latency, Motion-to-Photon, Signal Recovery Time, ICE Recovery Time, CPU, GPU, Private Bytes/Working Set, Crash Count, 8h/24h/72h stability.
- [ ] In `regression-matrix.md`, map video, mouse, keyboard, multi-monitor, clipboard, file transfer, authentication, P2P, TURN, reconnect, Ctrl+Alt+Del and Windows Service to existing automated smoke/tests plus required real-machine checks and rollback action.
- [ ] Commit: `prototype(rust): establish benchmark and regression gates`.

## Task 6: Property and stress tests for stale races

**Files:**
- Modify: `crates/desklink-session/Cargo.toml`
- Create: `crates/desklink-session/tests/generation_stress.rs`

- [ ] Add `proptest` as a dev dependency.
- [ ] Generate mixed current/stale session, peer, control, pointer and recovery events. Assert generations never decrease, stale events never mutate authoritative state, and stale channel closes never revoke newer replacements.
- [ ] Run `PROPTEST_CASES=10000 cargo test -p desklink-session --test generation_stress`; minimized counterexamples are treated as real bugs.
- [ ] Add a fixed-seed million-event stress test covering session restart, peer/channel replacement and recovery completion ordering.
- [ ] Run the stress test in `--release`; require PASS and record elapsed time as prototype evidence, not as a user-latency claim.
- [ ] Commit: `test(rust): stress stale lifecycle generations`.

## Task 7: Phase-1 decision gate

**Files:**
- Modify: `tools/rust-core/benchmark-baseline.md`
- Modify: `tools/rust-core/regression-matrix.md`
- Create: `docs/adr/ADR-001-rust-core-phase1-results.md`

- [ ] Run `cargo fmt --check`, strict clippy, all Rust tests, release-mode 10k-case property tests, session benchmarks and FFI benchmarks.
- [ ] Run unchanged production regression checks on the prototype branch: Signal tests/build, Web `npm ci`/audit/typecheck/Vitest/build, Windows Release build and every discovered native smoke, plus deployment smoke where branch rules allow.
- [ ] Record actual values, commit SHA, CI links, failures, memory/toolchain cost and every ADR acceptance criterion in the results document.
- [ ] End the results document with exactly `Decision: ADVANCE_TO_SHADOW_INTEGRATION` or `Decision: STOP_RUST_CORE_PROTOTYPE`.
- [ ] If STOP, do not create C++ FFI integration and do not merge Rust authority into `main`.
- [ ] Commit: `docs(rust): record phase one prototype results`.

## Task 8: Optional shadow integration only after ADVANCE decision

**Prerequisite:** Phase-1 results explicitly say `Decision: ADVANCE_TO_SHADOW_INTEGRATION`.

**Files:**
- Modify: `apps/windows-agent/CMakeLists.txt`
- Create: `apps/windows-agent/src/rust_core_shadow.h`
- Create: `apps/windows-agent/src/rust_core_shadow.cpp`
- Create: `apps/windows-agent/src/rust_core_shadow_smoke.cpp`
- Modify only normalized lifecycle call sites: `apps/windows-agent/src/webrtc_session.cpp`

- [ ] Write the native shadow smoke before adapter implementation; mirror normal connection lifecycle and stale replacement events without network/media dependencies.
- [ ] Verify `desklink-rust-core-shadow-smoke` initially fails because the target does not exist.
- [ ] Add `DESKLINK_ENABLE_RUST_CORE_SHADOW` CMake option default OFF. With OFF, Cargo/Rust must not be required for the normal Windows build.
- [ ] Implement an RAII wrapper owning exactly one opaque Rust handle. Mirror lifecycle events and log C++/Rust decision mismatches; C++ remains authoritative.
- [ ] Build and run the complete native smoke suite with shadow OFF and ON; both must PASS.
- [ ] Exercise real connect, signal reconnect, ICE restart, peer/channel replacement, lock/unlock and Service restart; any unexplained decision mismatch blocks authority transfer.
- [ ] Commit shadow integration separately as `prototype(rust): mirror session lifecycle in shadow core`.

---

## Plan Self-Review

- ADR-001 context, FFI boundary, generation authority, recovery, benchmark gate, regression matrix, clean-room reference, strangler migration, rollback and stop criteria all map to explicit tasks.
- No task replaces the current Windows media/input stack, libdatachannel, Go Signal or Web Controller in Phase 1.
- Recovery remains deterministic and runtime-agnostic; Tokio is not introduced prematurely.
- Tasks 1-7 do not give Rust production authority. Task 8 is optional and shadow-only with CMake default OFF.
- Every implementation task begins with a failing test or measurable contract and ends with explicit verification and an isolated commit.
