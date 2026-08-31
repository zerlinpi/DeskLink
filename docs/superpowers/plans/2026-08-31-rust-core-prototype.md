# Rust Core Main-Only Incremental Migration Plan

> **Execution rule:** all implementation, tests, benchmarks, documents and architecture migration happen on `main`. Do not create a prototype, feature, experiment, migration or PR branch for this plan.

**Goal:** move DeskLink's highest-risk lifecycle coordination toward a strongly typed Rust core without destabilizing the mature Windows media/input stack.

**Architecture:** C++ remains production authority while Rust first runs as a deterministic tested core, then as a default-OFF shadow observer. Authority transfer is allowed only after shadow evidence is clean and occurs one responsibility at a time with rollback.

**Spec:** `docs/adr/ADR-001-rust-core.md`

## Global Constraints

- Work only on `main`.
- Never force-push or replace unrelated user changes.
- Do not rewrite DXGI, D3D11, Media Foundation, SendInput, libdatachannel/WebRTC, Windows Service, Go Signal or the React Controller merely to increase Rust usage.
- Existing C++/TypeScript/Go regression tests remain the production oracle.
- Rust panic and C++ exceptions never cross the C ABI.
- Stale generations fail closed.
- CI validates code; CI never commits or pushes product source.
- Every authority move keeps a rollback mechanism until stable.

## Completed Phase 1

Phase-1 evidence and measured results are in `docs/adr/ADR-001-rust-core-phase1-results.md`.

Implemented on `main`:

- Cargo workspace and pinned toolchain;
- strongly typed generation model;
- deterministic `RemoteSessionStateMachine`;
- deterministic `RecoveryCoordinator`;
- panic-safe versioned C ABI;
- unit, property and million-event stress tests;
- Criterion lifecycle and FFI benchmark baselines;
- default-OFF Windows Rust shadow build path;
- native shadow adapter, lifecycle adapter and callback-ordering event bridge.

The Phase-1 result ends with:

```text
Decision: ADVANCE_TO_SHADOW_INTEGRATION
```

That decision authorizes only shadow integration. C++ authority remains unchanged.

## Current Task: Production Shadow Wiring

### Task 1 — Normalize repository rules to main-only

- [x] ADR states `main-only incremental migration`.
- [x] This implementation plan no longer instructs creation of a prototype branch.
- [x] Active Rust CI runs for `main` pushes/PRs.
- [x] Active shadow CI runs for `main` changes.

### Task 2 — Mirror current C++ lifecycle decisions

**Files:**

- `apps/windows-agent/src/webrtc_session.h`
- `apps/windows-agent/src/webrtc_session.cpp`
- `apps/windows-agent/src/rust_core_shadow.h`
- `apps/windows-agent/src/rust_core_shadow.cpp`
- `apps/windows-agent/src/rust_core_shadow_event_bridge.cpp`
- `apps/windows-agent/src/rust_core_shadow_smoke.cpp`

Requirements:

- [ ] `WebRtcSession` owns the callback-ordering `RustCoreShadowEventBridge`, not a second production authority.
- [ ] A newly accepted C++ peer starts a new normalized Rust shadow session; an explicit same-session peer replacement keeps the Session generation and advances Peer generation.
- [ ] Peer `Connected` callbacks compare current vs stale C++ authority before Rust sees the event.
- [ ] Control and Pointer open/close callbacks mirror the exact `InputChannelAuthority` accepted/rejected result.
- [ ] Channel-open-before-peer-connected ordering is buffered by `RustCoreShadowEventBridge` and replayed after PeerConnected.
- [ ] Session stop/revocation closes or resets only the observer after C++ authority has already been cleared.
- [ ] A Rust mismatch increments diagnostics but never changes the C++ branch taken by production code.

### Task 3 — Preserve isolation and rollback

- [x] `DESKLINK_ENABLE_RUST_CORE_SHADOW` defaults `OFF`.
- [x] OFF builds do not require Cargo/Rust.
- [ ] ON builds link the Rust static library and run the observer without changing production authority.
- [ ] Shadow can be disabled quickly without changing protocol or persisted state.

### Task 4 — Shadow verification

Required automated checks:

```text
cargo fmt --check
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace --all-targets
```

Windows CI must verify both:

1. default shadow OFF full Release build + all native smokes without invoking Cargo;
2. shadow ON Rust static library + shadow smoke + full Release build + all native smokes.

Shadow smoke must cover:

- normal lifecycle;
- stale Session/Peer/Control/Pointer/Operation events;
- peer replacement;
- channel replacement;
- channel-open-before-peer-connected callback ordering;
- concurrent mismatch accounting.

### Task 5 — Real-machine gate before authority transfer

Do not transfer authority until real Windows/client/network testing records:

- zero unexplained C++/Rust lifecycle mismatches;
- connection success and TTFF shadow OFF vs ON;
- Signal/ICE recovery and session replacement distributions;
- input latency / Motion-to-Photon;
- Agent CPU and Private Bytes / Working Set delta;
- Intel/NVIDIA/AMD and high-refresh regression matrix;
- real P2P and forced TURN relay;
- actual Ctrl+Alt+Del on supported Win10/11 policy configurations;
- 8h, 24h and 72h soak.

## After Shadow Passes

Authority may move incrementally, still on `main`, in this order:

1. Generation / stale validation.
2. Recovery policy.
3. `RemoteSessionStateMachine`.
4. Auth/session coordination.

For each authority move:

- old C++ fallback remains temporarily;
- a feature/runtime switch provides rollback;
- existing product capabilities and regressions must stay green;
- remove the old implementation only after a stable release.

## Stop Rule for This Round

After production Shadow wiring, automated verification and an accurate status report, stop. Do not continue directly into LAN Direct, Audio, Secure Desktop, media backend replacement or Rust production authority.
