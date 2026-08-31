# ADR-001: Evidence-Gated Rust Core Prototype

- **Status:** Accepted for prototype only
- **Date:** 2026-08-31
- **DeskLink baseline:** `f248fe1d6a4e22b09bfaa9154ac9f3b8e41c39a7`
- **Target prototype branch:** `prototype/rust-core`
- **Production impact:** None until benchmark and regression gates pass

## Context

DeskLink currently uses a mixed implementation:

- Windows C++20 Agent and LocalSystem Service
- DXGI Desktop Duplication
- D3D11 GPU processing
- Media Foundation H.264
- libdatachannel/WebRTC
- Go Signal/Auth server
- React/TypeScript Web Controller

This architecture already provides the working remote-control data plane and must remain the production reference while architecture experiments are evaluated.

The areas with the highest bug and maintenance risk are no longer the mature Windows media hot path. They are lifecycle and coordination concerns such as session generation, stale asynchronous callbacks, reconnect and recovery, authentication state, privileged-operation scoping, signaling races, file-transfer lifecycle, updater state, IPC, and device lifecycle.

The project therefore needs an evidence-driven way to evaluate whether Rust can reduce concurrency and lifetime risk without sacrificing connection success, latency, compatibility, or operational simplicity.

## Current Architecture

The production Windows Host currently combines several responsibilities around the WebRTC session:

- signaling and token loading
- authentication and challenge handling
- PeerConnection lifecycle
- ICE/TURN configuration and recovery
- reliable Control DataChannel
- unreliable Pointer DataChannel
- clipboard and file transfer
- privileged Service coordination
- host capability publication
- media/RTP coordination

DeskLink has already added generation and scope guards in C++/TypeScript to prevent stale peer, stale channel, stale negotiation, and stale session callbacks. Those guards are valuable and remain the production regression reference.

Windows media and input paths currently use native C++ APIs directly:

- DXGI Desktop Duplication
- D3D11 textures and conversion
- Media Foundation hardware H.264
- Win32 SendInput
- Windows Service/session APIs

These paths are not migration targets for the first prototype.

## Candidate Architecture

Adopt a **Strangler Rust Core** prototype while keeping the current production media stack intact.

```text
DeskLink
├─ crates/
│  ├─ desklink-protocol
│  ├─ desklink-session
│  ├─ desklink-recovery
│  ├─ desklink-auth
│  └─ desklink-core-ffi
│
├─ apps/windows-agent/        # current C++ production backend
│  ├─ DXGI
│  ├─ D3D11
│  ├─ Media Foundation
│  ├─ SendInput
│  ├─ Windows Service APIs
│  └─ libdatachannel/WebRTC
│
├─ apps/signal/               # Go remains production server in Phase 1
└─ apps/web/                  # React/WebRTC remains production controller
```

The first Rust prototype owns only deterministic coordination state:

- `RemoteSessionStateMachine`
- `RecoveryCoordinator`
- generation management
- protocol/domain types that do not require platform media APIs
- authentication state transitions
- FFI-safe commands/events between C++ and Rust

The first prototype does **not** replace DXGI, D3D11, Media Foundation, SendInput, libdatachannel, the Windows Service process model, the Go Signal server, or the Browser WebRTC controller.

## Session Model

The Rust prototype will model explicit session states rather than allowing callback-local state transitions:

```text
Idle
  -> Signaling
  -> Authenticating
  -> Negotiating
  -> Connected
  -> RecoveringSignal | RecoveringTransport
  -> Connected
  -> Closing
  -> Idle
```

State-changing asynchronous work must be scoped by generation tokens. At minimum the prototype will model:

- `SessionId`
- `SessionGeneration`
- `PeerGeneration`
- `ControlChannelGeneration`
- `PointerChannelGeneration`
- `OperationGeneration`

A late callback may report an event, but the Rust state machine decides whether the event is still authoritative. Stale generations must fail closed without mutating current session state.

## FFI Boundary

The prototype FFI must remain narrow and deterministic.

C++ sends input events such as:

- signaling connected/disconnected
- authentication accepted/rejected
- peer created/replaced/failed
- control channel opened/closed
- pointer channel opened/closed
- ICE failed/recovered
- recovery timeout
- session close requested

Rust returns commands such as:

- begin authentication
- begin negotiation
- schedule signaling reconnect
- schedule ICE restart
- invalidate peer generation
- invalidate input-channel authority
- close session
- ignore stale event

Rules:

1. No Rust object pointer may be retained by arbitrary C++ callbacks without an opaque ownership handle.
2. No C++ exception may cross the FFI boundary.
3. No Rust panic may cross the FFI boundary.
4. FFI messages must use fixed-width, versioned C-compatible representations or serialized protocol messages with explicit size limits.
5. Allocation ownership must be explicit at every boundary.
6. C++ remains responsible for Windows COM/D3D/MF object lifetime in Phase 1.

## Reference Projects

### RustDesk

Reference snapshot reviewed: RustDesk `master` at commit `03a7fc5992069cc5bc9f7c36b872483dddf4f472`.

Relevant architectural evidence:

- Rust workspace with reusable library boundaries for capture, input, clipboard, common networking/types, virtual display, and portable platform code.
- Core library can be emitted as `cdylib`, `staticlib`, and `rlib`, demonstrating a viable Rust-core/native-boundary model.
- `hbb_common` centralizes Tokio, protocol serialization, TLS/WebSocket/network primitives, configuration, and shared models.
- rendezvous and relay responsibilities are modeled independently rather than as one monolithic transport.
- Windows code uses Rust Windows ecosystem crates while still retaining platform-specific implementations.
- LAN, IPC, updater, service coordination, clipboard, file transfer, and rendezvous logic are largely outside the media capture hot path.

RustDesk is licensed under **AGPL-3.0**. DeskLink may study its public architecture and behavior, but the prototype is a clean-room implementation. No RustDesk source code is to be copied into DeskLink unless a separate explicit license decision is made.

### DeskLink production baseline

DeskLink production behavior at the baseline commit remains the regression oracle. Existing C++/TypeScript generation, peer-scope, input-authority, recovery, authentication, and native smoke tests must not be removed simply because equivalent Rust logic exists in the prototype.

## Benefits

Potential benefits to be measured, not assumed:

- stronger ownership and lifetime guarantees for session state
- fewer stale-callback races
- deterministic generation validation in one core instead of duplicated callback checks
- easier property and stress testing of recovery behavior without Windows media dependencies
- possible future protocol/auth reuse with Native Controller or Signal server
- clearer separation between platform media backend and product/session state
- safer foundation for updater, LAN discovery, IPC, and native controller work

## Costs

- additional Rust toolchain and CI time
- C++/Rust FFI complexity
- two implementations must coexist during strangler migration
- debugging spans C++ and Rust until migration stabilizes
- shared protocol ownership can become more complex if bindings/code generation are introduced prematurely
- additional packaging work for static library/runtime integration
- migration can distract from product work if benchmark gates are not enforced

## Performance Requirements

Rust is not accepted because of language-level safety claims alone.

The prototype must benchmark against the existing C++ coordination path where comparison is meaningful.

### Synthetic benchmarks

Measure at minimum:

- state-transition throughput
- event dispatch latency p50/p95/p99
- stale-event rejection throughput
- 100k and 1M generated lifecycle events
- memory allocation count/bytes where measurable
- steady-state resident memory overhead
- FFI call overhead
- recovery timer scheduling overhead

### Product benchmarks

On real Windows machines compare:

- Connection Success Rate
- Time To First Frame
- input latency
- Motion-to-Photon latency where telemetry exists
- signaling recovery time
- ICE recovery time
- session replacement time
- CPU
- GPU impact (must be statistically unchanged in Phase 1)
- memory
- crash rate
- 8h, 24h, and 72h stability

### Acceptance gate

A migration phase may merge into production only if:

- no core regression-matrix capability is lost;
- connection success is not lower beyond normal test variance;
- TTF and recovery time are not materially worse;
- input latency does not regress materially;
- memory overhead is understood and acceptable;
- no new crash/lifetime class is introduced at the FFI boundary;
- race/stale-callback stress tests are at least as strong as the current implementation;
- rollback remains possible in one release.

No code-line-count metric is an acceptance criterion.

## Regression Matrix

Every migration phase must preserve the current production capability baseline.

| Capability | Production reference | Prototype requirement |
| --- | --- | --- |
| Video | Existing DXGI/D3D11/MF/WebRTC path | unchanged |
| Mouse | Pointer DataChannel + SendInput | unchanged semantics |
| Keyboard | reliable Control + SendInput | unchanged semantics |
| Multi-monitor | existing monitor state/switch | unchanged |
| Clipboard | existing reliable control flow | unchanged |
| File transfer | existing chunk/download policy | unchanged |
| Authentication | existing Access Code/device/controller auth | equivalent or stronger |
| P2P | existing ICE/WebRTC | unchanged |
| TURN | existing TURN fallback | unchanged |
| Reconnect | existing signal/ICE recovery | equivalent or faster |
| Ctrl+Alt+Del | existing authenticated SAS Broker | unchanged |
| Windows Service | existing LocalSystem + Session Agent | unchanged in Phase 1 |

Features that are not production-ready before migration are not counted as regressions merely because the prototype does not implement them.

## Security

The Rust core is not a new privileged broker.

- LocalSystem Service privilege boundaries remain unchanged in Phase 1.
- SAS and other privileged operations continue to use the existing authenticated Service Broker.
- Access Code, Device Credential, Controller Token, Signal Token, and TURN credentials must not cross the FFI boundary unless required by an explicit auth operation.
- Secret-bearing buffers must have explicit ownership and clearing behavior.
- generation IDs are authorization context, not cryptographic credentials.
- privileged operation allowlists remain enforced by the existing broker.
- no arbitrary command execution, arbitrary file path RPC, DLL loading, or process-launch primitive is introduced.

## Compatibility

The production branch continues to build and run without Rust Core enabled until an explicit migration phase is accepted.

Prototype integration should use a feature/build switch such as a CMake option rather than replacing the C++ implementation in place.

Target compatibility during Phase 1:

- current Windows Agent remains default
- current Web Controller remains unchanged
- current Go Signal server remains unchanged
- wire protocol remains backward compatible
- Rust core may consume internal normalized events but must not introduce a new incompatible public transport protocol

## Migration Complexity

### Phase 0 — Baseline

- freeze benchmark definitions
- capture current C++ state/recovery behavior
- document regression matrix
- ensure all existing native/Web/Go tests remain green

### Phase 1 — Rust protocol/session/recovery prototype

- create Rust workspace on `prototype/rust-core`
- implement deterministic state machine and generations
- implement recovery coordinator
- define narrow C ABI
- run synthetic benchmark and stress tests
- no production path replacement

### Phase 2 — Optional shadow integration

Only if Phase 1 passes:

- C++ continues driving production behavior
- Rust core receives mirrored lifecycle events
- compare C++ decision vs Rust decision
- record mismatches without changing user-visible behavior

### Phase 3 — Selective authority transfer

Only if shadow results pass:

- enable Rust authority for one bounded responsibility behind a build/runtime flag
- candidate: generation/scope validation or recovery scheduling
- preserve C++ fallback

### Phase 4 — Broader core migration

Only after real-machine and soak results justify it:

- protocol/auth/session/network coordination may move to Rust incrementally
- Go Signal, WebRTC implementation, and Windows media remain separate ADR decisions

## Rollback Plan

Every phase must be removable without rewriting the media stack.

- C++ production implementation stays available until a later ADR explicitly retires it.
- Rust authority is enabled behind an explicit build/runtime switch during transition.
- wire formats remain compatible.
- release packaging can revert to C++-only in one release.
- no persistent on-disk state is migrated to a Rust-only format in Phase 1.
- if benchmark gains are insufficient, stop after the prototype and delete the experimental authority path without impacting production data.

## Alternatives Considered

### A. Full Rust rewrite now

Rejected.

It mixes language migration, media rewrite, networking rewrite, packaging changes, and platform API changes. Any regression would be difficult to attribute, and DeskLink would lose its working rollback path.

### B. Rust WebRTC and networking first

Deferred.

This could eventually reduce callback/lifetime complexity, but replacing libdatachannel while also introducing Rust would confound transport and language benchmark results. Browser interoperability and ICE/TURN behavior are production-critical and should remain stable during the first core experiment.

### C. Rust Signal server first

Deferred.

Go currently provides a working Signal/Auth server with substantial tests. A Rust rewrite has little value until shared Rust protocol/auth/session crates actually exist and demonstrate concrete reuse. Server language is not currently the highest-risk component.

### D. Rust Core + C++ Windows media backend

Selected for prototype.

It targets the highest concurrency/lifetime-risk area while preserving the mature GPU/media/input path and the existing browser/server ecosystem.

## Future ADRs

The following decisions remain independent and require their own evidence:

- `ADR-002-webrtc-vs-native-quic.md`
- `ADR-003-dxgi-vs-wgc.md`
- `ADR-004-go-signal-vs-rust-signal.md`
- Native Controller framework and transport choice
- encoder backend abstraction

No decision in this ADR pre-approves those migrations.

## Decision

Proceed with a **prototype-only Strangler Rust Core**.

The first implementation branch will be `prototype/rust-core` and will initially contain only protocol/session/recovery/auth coordination plus a narrow FFI boundary and benchmark/regression tooling.

The C++ Windows media backend, libdatachannel/WebRTC transport, Go Signal server, React Web Controller, Service architecture, and wire behavior remain the production reference during the prototype.

The prototype may advance to shadow integration only after benchmark and regression gates are met. If the data does not show a meaningful product or engineering benefit, the migration stops with no production rewrite.
