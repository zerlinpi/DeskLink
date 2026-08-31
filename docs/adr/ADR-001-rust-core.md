# ADR-001: Main-Only Incremental Rust Session/Recovery Core

- **Status:** Accepted for incremental migration
- **Date:** 2026-08-31
- **Migration mode:** `main-only incremental migration`
- **Current production authority:** C++
- **Current Rust stage:** shadow-only integration is authorized; Rust production authority is not

## Decision

DeskLink will evaluate and adopt Rust incrementally for deterministic session, generation, recovery, protocol/domain and operation-lifecycle coordination while preserving the mature Windows media/input stack.

All implementation, tests, experiments, benchmarks, documentation and migration work happen directly on `main`. No prototype, feature, experiment or migration branch is required or permitted for this effort.

Rust work on `main` is isolated from production authority through layered gates:

1. the CMake option `DESKLINK_ENABLE_RUST_CORE_SHADOW`, default `OFF`;
2. C++ remains the authoritative decision maker during shadow integration;
3. Rust receives normalized lifecycle events and compares decisions only;
4. mismatch telemetry blocks authority transfer;
5. any later authority transfer is incremental and keeps a rollback switch until stable.

## Context

DeskLink already has a working mixed stack:

- Windows C++20 Agent and LocalSystem Service;
- DXGI Desktop Duplication and D3D11 processing;
- Media Foundation hardware H.264;
- libdatachannel/WebRTC with STUN/TURN/ICE;
- reliable Control and Pointer DataChannels;
- Win32 SendInput, clipboard, file transfer and multi-monitor support;
- Access Code, device/controller authentication and DPAPI storage;
- Secure Attention broker;
- Go Signal/Auth server;
- React/TypeScript Web Controller;
- generation/scope guards, reconnect/recovery logic, telemetry and regression tests.

The first Rust migration target is not the media hot path. It is lifecycle coordination where stale asynchronous callbacks, replacement races and recovery overlap are most difficult to reason about.

## Rust Core Scope

The workspace contains:

```text
Cargo.toml
Cargo.lock
rust-toolchain.toml
crates/
  desklink-protocol/
  desklink-session/
  desklink-recovery/
  desklink-core-ffi/
```

Phase 1 Rust owns deterministic coordination models only:

- strongly typed generations;
- `RemoteSessionStateMachine`;
- `RecoveryCoordinator`;
- protocol/domain lifecycle types;
- operation lifecycle;
- a narrow panic-safe C ABI.

Phase 1 does not replace DXGI, D3D11, Media Foundation, SendInput, Windows Service process boundaries, libdatachannel/WebRTC, the Go Signal server, or the Browser Controller.

## Generation Rules

The model uses separate generation types for session, peer, control channel, pointer channel, negotiation, recovery and operation scopes where applicable.

Generations:

- start from explicit non-zero values;
- advance monotonically;
- fail closed on overflow;
- are never interchangeable raw lifecycle identifiers;
- are checked before an asynchronous completion may mutate authority.

A stale callback may be recorded for diagnostics but must not change current session state, UI state, input authority, signaling, channel ownership or recovery authority.

## Session State Machine

Lifecycle decisions are normalized as:

```text
Event -> RemoteSessionStateMachine -> Commands
```

rather than callback-local mutation.

The model covers the connection lifecycle, replacement, closing and recovery-related states required by the current DeskLink coordination path. A connected PeerConnection alone is not equivalent to full remote readiness; authentication and current channel/capability authority remain separate conditions.

## Recovery Coordinator

Recovery policy is deterministic and runtime-independent. It coordinates escalating recovery actions rather than allowing independent watchdogs to race each other.

The intended escalation remains:

1. natural recovery;
2. DataChannel recreation;
3. ICE restart;
4. PeerConnection rebuild;
5. Signal reconnect;
6. full session rebuild.

Only one authoritative recovery operation may own a recovery generation at a time. Actual timers and transport operations remain outside the Rust reducer until a later authority-transfer gate explicitly moves them.

## FFI Boundary

The Rust/C++ boundary is intentionally narrow:

- opaque handle ownership;
- versioned fixed-width C structures;
- explicit allocation ownership;
- no Rust `Vec`, `String`, references, trait objects or enum layout exposed directly;
- every exported Rust entrypoint catches panics;
- C++ exceptions and Rust panics never cross the ABI boundary.

## Shadow Mode

`DESKLINK_ENABLE_RUST_CORE_SHADOW` defaults to `OFF`.

### OFF

- the normal Windows build does not require Cargo/Rust;
- the existing C++ implementation retains complete authority;
- user-visible behavior is unchanged.

### ON

- C++ still makes the real production decision;
- the same accepted/rejected normalized lifecycle decision is mirrored to Rust;
- session/peer/channel generations are included in comparisons;
- callback reordering is normalized by the shadow event bridge;
- mismatches are logged and counted;
- Rust must never change user-visible behavior because of a mismatch.

Shadow mismatches block production authority transfer until explained and fixed.

## Testing and Evidence

Rust Core changes require:

```text
cargo fmt --check
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace --all-targets
```

Property/stress coverage includes at least 10,000 generated orderings and million-event stale/replacement stress in release mode.

Existing Web, Go, Windows native, protocol and deployment regression suites remain mandatory. Rust tests add protection; they do not replace existing regressions.

Synthetic benchmarks measure lifecycle throughput, stale-event handling, replacement paths and C ABI overhead. Real-machine connection success, TTFF, input latency, recovery, memory, GPU, TURN/P2P and soak evidence remain required before any authority transfer.

Phase-1 evidence is recorded in `docs/adr/ADR-001-rust-core-phase1-results.md`, whose decision is `ADVANCE_TO_SHADOW_INTEGRATION`. That decision authorizes shadow-only integration, not Rust production authority.

## Authority Transfer

If shadow evidence passes, authority may move gradually on `main`:

1. generation/stale validation;
2. recovery policy;
3. remote session state machine;
4. auth/session coordination.

Each transfer keeps the C++ fallback behind a rollback switch until it is stable for at least one release. No all-at-once C++ -> Rust rewrite is approved by this ADR.

## Media and Server Boundaries

Rust is not a goal by itself. The current C++ Windows media backend and Go Signal server remain valid long-term components unless separate benchmarks show a meaningful product or engineering benefit from replacement.

Future DXGI vs WGC, encoder backend, WebRTC vs QUIC/custom transport, Go vs Rust Signal and Native Controller decisions remain independent evidence-gated decisions.

## Open-Source Reference Policy

DeskLink may study architecture, state machines, algorithms, module boundaries and behavior in projects such as RustDesk, Sunshine, Moonlight, MeshCentral, Apache Guacamole and noVNC.

License compatibility must be checked before adding third-party source. RustDesk remains clean-room architectural reference only unless an explicit license decision says otherwise.

## Rollback

At every migration stage:

- C++ remains available until explicitly retired;
- normal builds can remain Rust-free while shadow is disabled;
- wire formats and persisted production state remain compatible;
- a failed shadow or authority experiment can be disabled without rewriting the media stack.

This ADR therefore approves a **main-only, evidence-gated, incremental Rust Core migration** with C++ authority retained through the current shadow phase.
