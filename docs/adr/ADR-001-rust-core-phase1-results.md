# ADR-001 Phase-1 Results: Rust Core Prototype

- **Date:** 2026-08-31
- **ADR:** `docs/adr/ADR-001-rust-core.md`
- **Prototype branch:** `prototype/rust-core`
- **Production baseline:** `f248fe1d6a4e22b09bfaa9154ac9f3b8e41c39a7`
- **Phase-1 gate commit:** `bb0e4d43104d52b75cf3e725b5f93e5edb50d180`
- **Successful Phase-1 gate:** GitHub Actions run `33395660025`
- **First Phase-1 gate with deployment failure:** run `33392595469`
- **Production authority:** unchanged; C++ remains authoritative

## Scope of this decision

This document decides only whether the isolated Rust coordination prototype has enough evidence to proceed to **shadow integration**. It does not approve Rust production authority, does not replace any media/input/networking implementation, and does not claim real-machine product metrics that Phase 1 could not measure because Rust is not yet linked into the Windows Agent production path.

A positive decision therefore authorizes only Task 8's default-OFF shadow adapter: C++ continues making every production decision; Rust receives mirrored normalized lifecycle events and reports mismatches. Any later authority transfer requires a separate evidence gate with real-machine product and soak results.

## Implemented Phase-1 surface

Phase 1 produced:

- strongly typed monotonic Session / Peer / Control / Pointer / Operation generations;
- deterministic `RemoteSessionStateMachine` lifecycle decisions;
- generation high-water semantics that prevent closed/expired authority from being resurrected by late callbacks;
- deterministic, runtime-independent recovery coordination;
- versioned, fixed-width, panic-safe C ABI with opaque ownership;
- synthetic Criterion benchmarks for reducer and FFI paths;
- deterministic stale-generation tests, 10,000-case property tests, and a fixed-seed 1,000,000-event race stress;
- a full production regression matrix and repeatable Phase-1 full-stack GitHub Actions gate.

Phase 1 did **not** replace DXGI, D3D11, Media Foundation, SendInput, libdatachannel/WebRTC, Windows Service behavior, Go Signal, React Web Controller, wire formats, or persistent production state.

## Correctness evidence

### Stale generation RED -> GREEN

The Phase-1 property work found four concrete authority-resurrection cases in the prototype reducer:

1. a closed Session generation could be accepted again;
2. a closed Control generation could be accepted again;
3. a closed Pointer generation could be accepted again;
4. a timed-out Operation generation could be accepted again.

Run `33390962135` provided the effective RED: formatting and strict Clippy passed, then exactly the four new high-water tests failed. The minimal fix retained a global Session high-water mark and scoped Control / Pointer / Operation high-water marks, resetting scoped high-water only when entering a new Session/Peer scope. Run `33391252769` then passed fmt, strict Clippy, tests, and Release FFI build.

Subsequent permanent property/stress evidence:

- run `33391990346`: `property_stress.rs` 10,000-case property and 1,000,000-event fixed-seed stress both PASS;
- run `33392255588`: permanent Release property/stress CI gate PASS;
- run `33395660025`: same-SHA Phase-1 gate repeats Release property/stress PASS.

On run `33395660025`, the release 10,000-case property test body completed in about `0.01s`; the release 1,000,000-event stress test body completed in about `0.04s`, with shell timing including Cargo startup of `0.136s`. These are reducer stress results, not user-latency measurements.

### FFI contract

The C ABI tests cover fixed-width layout, null arguments, unsupported ABI version, invalid event kind, current/stale status mapping, command-buffer clearing, 10,000 create/destroy cycles, and a deliberately forced Rust panic. The panic is caught at the ABI boundary and maps to the stable PANIC status rather than unwinding into C++.

The Phase-1 gate repeats all workspace tests and a Release `desklink-core-ffi` build successfully.

## Synthetic performance evidence

The first independent baseline is recorded in `tools/rust-core/benchmark-baseline.md`. The Phase-1 gate independently repeated Criterion on GitHub-hosted Ubuntu 24.04. Hosted-runner absolute values drift across machines, so the samples establish scale and repeatability rather than a cross-run hard regression threshold.

### Session reducer, Phase-1 gate run `33395660025`

| Scenario | 100k median | 1M median | 1M throughput median |
| --- | ---: | ---: | ---: |
| valid lifecycle | `2.6978 ms` | `29.150 ms` | `34.305 Melem/s` |
| stale events | `2.4605 ms` | `24.635 ms` | `40.593 Melem/s` |
| peer replacement | `2.6585 ms` | `26.593 ms` | `37.604 Melem/s` |
| control replacement | `1.2304 ms` | `12.315 ms` | `81.203 Melem/s` |
| pointer replacement | `1.2307 ms` | `12.300 ms` | `81.302 Melem/s` |

### Direct reducer vs C ABI

| Path | Event | 100k median | 1M median | Derived FFI overhead at 1M |
| --- | --- | ---: | ---: | ---: |
| Direct | current | `1.2378 ms` | `12.398 ms` | baseline |
| C ABI | current | `1.6854 ms` | `16.929 ms` | `~4.531 ns/event` |
| Direct | stale | `2.4099 ms` | `23.991 ms` | baseline |
| C ABI | stale | `2.5203 ms` | `25.756 ms` | `~1.765 ns/event` |

The current-event FFI path is measurably slower than the direct reducer in percentage terms, but the measured synthetic absolute boundary cost remains single-digit nanoseconds per event on these Linux hosted runs. That does not establish Windows end-to-end latency and is not used to claim user-visible performance improvement.

## Toolchain and memory cost

Measured CI/toolchain cost on run `33395660025` included approximately:

- pinned Rust 1.85.1 toolchain installation: about 8 seconds on the hosted image;
- cold strict Clippy dependency/download/build phase: about `13.4s`;
- first release property-test compile: about `30.3s`;
- session bench profile incremental build: about `1.8s`;
- additional FFI bench build: about `13.1s`.

These values are CI costs, not runtime overhead.

Phase 1 intentionally has no Windows Agent integration, so it cannot produce a meaningful Rust incremental Agent Private Bytes / Working Set measurement. Runtime memory overhead is therefore **not yet measured**, not assumed to be zero. Task 8 shadow integration must collect Agent/Service memory before any proposal for production authority transfer.

## Production regression gate

Run `33395660025` completed all five jobs successfully at the same commit.

### Rust

PASS:

- `cargo fmt --check`;
- strict Clippy with `-D warnings`;
- workspace `--all-targets` tests;
- Release 10,000-case property test;
- Release 1,000,000-event stress;
- session Criterion benchmark;
- FFI Criterion benchmark;
- Release FFI build.

### Signal

PASS:

- Go dependency resolution;
- `go test ./...`;
- build with repository version;
- legacy device credential provisioning;
- independent device registry credential rotation;
- controller registry provisioning/rotation.

### Web Controller

PASS:

- Access Code proof vector;
- locked `npm ci`;
- `npm audit --audit-level=moderate`;
- TypeScript typecheck;
- unit tests;
- production build.

### Windows Agent / Service

Run `33395660025` used a GitHub-hosted Windows Server 2025 runner and passed:

- packaging PowerShell syntax validation;
- CMake configure;
- static `/MT` OpenSSL selection validation;
- complete Release build;
- portable dependency validation for `DeskLink.exe`, `desklink-agent.exe`, `desklink-service.exe`, and `desklink-media-probe.exe`;
- machine-scope DPAPI device credential/access-code store and clear;
- all dynamically discovered native smoke targets.

The native smoke runner executed 13 targets, all PASS:

1. `desklink-access-proof-smoke`
2. `desklink-active-session-smoke`
3. `desklink-device-identity-smoke`
4. `desklink-file-transfer-chunk-policy-smoke`
5. `desklink-file-transfer-download-policy-smoke`
6. `desklink-h264-annexb-smoke`
7. `desklink-host-capabilities-smoke`
8. `desklink-input-channel-authority-smoke`
9. `desklink-peer-signal-scope-smoke`
10. `desklink-pointer-wire-smoke`
11. `desklink-secure-attention-smoke`
12. `desklink-service-auth-smoke`
13. `desklink-video-policy-smoke`

The SAS evidence here validates Secure Attention API resolution and authenticated Service broker capability plumbing. A hosted runner did not prove that a real Win10/11 machine with the required local policy visibly presents Ctrl+Alt+Del, and it does not prove Secure Desktop video/control.

### Production deployment smoke

The first Phase-1 gate, run `33392595469`, failed before deployment startup because `infra/production/bootstrap.sh` is stored with Git mode `100644` while the gate called it directly as `./bootstrap.sh`, producing exit `126` / permission denied. This was treated as a deployment usability defect rather than ignored.

The supported invocation and deployment documentation were changed to `sh bootstrap.sh ...`. The second gate then PASSed:

- repository version invariants;
- HostCapabilitiesV1 schema fixture;
- shell syntax;
- development and production Compose rendering;
- isolated production bootstrap producing `.env` and credential registries;
- actual production build/start of Signal, Web, and coturn;
- Signal `/readyz`;
- Web HTTP health;
- Signal/Web/TURN containers running;
- TURN TCP port `3478` reachable locally;
- teardown/volume cleanup.

This is a local hosted deployment smoke. It is not evidence of public DNS/TLS, NAT traversal, two-host P2P success, or real TURN relay behavior across networks.

## ADR-001 acceptance criteria evaluation

| Criterion | Phase-1 result | Interpretation |
| --- | --- | --- |
| No core regression-matrix capability is lost | **PASS for automated Phase-1 scope** | Full unchanged Signal/Web/Windows/deployment automated regression gate passed; real-machine rows remain open. |
| Connection success not lower beyond normal variance | **NOT MEASURABLE IN PHASE 1** | Rust is not in the production path. Must be compared with shadow enabled/disabled on real machines before authority transfer. |
| TTFF and recovery not materially worse | **NOT MEASURABLE IN PHASE 1** | Synthetic recovery semantics pass; real Signal/ICE recovery and TTFF require integrated shadow instrumentation. |
| Input latency does not regress materially | **NOT MEASURABLE IN PHASE 1** | SendInput and DataChannels remain C++; real input/Motion-to-Photon measurement is a Task 8+ gate. |
| Memory overhead understood and acceptable | **PARTIAL** | CI/toolchain cost is recorded; production runtime memory delta cannot exist as a meaningful measurement until Rust is linked into Agent shadow. Must be measured before authority transfer. |
| No new crash/lifetime class at FFI boundary | **PASS for prototype contract** | Panic containment, opaque ownership, invalid/null ABI handling, 10k create/destroy and Release build all pass. Integrated native lifetime still requires shadow soak. |
| Race/stale-callback stress at least as strong as current implementation | **PASS for modeled lifecycle scope** | Deterministic high-water regressions, 10k property and 1M stale-race stress pass repeatedly. |
| Rollback possible in one release | **PASS** | Phase 1 has no production integration. Task 8 is required to use a CMake option default OFF with C++ authoritative and no Rust requirement when OFF. |

The criteria marked NOT MEASURABLE/PARTIAL are **not waived**. They prevent production authority transfer, but they do not prevent a default-OFF shadow integration whose purpose is to collect exactly those integrated product and memory measurements while leaving C++ authoritative.

## Remaining hard gates before any authority transfer

Shadow integration must still demonstrate, on real Windows/client/network environments:

- zero unexplained C++/Rust lifecycle decision mismatches during normal connect, disconnect, peer/channel replacement, Signal reconnect, ICE restart, lock/unlock and Service restart;
- connection success, TTFF, Signal recovery, ICE recovery and session replacement distributions with shadow OFF vs ON;
- input latency / Motion-to-Photon with no material regression;
- Agent/Service CPU and Private Bytes / Working Set delta;
- Intel/NVIDIA/AMD capture/encode paths and supported high-refresh behavior unaffected;
- real P2P and forced TURN relay across representative NAT/network conditions;
- actual Ctrl+Alt+Del on supported Win10/11 policy configurations;
- 8h, 24h and 72h soak with crash count and resource-growth tracking;
- a verified one-switch rollback to C++-only behavior.

Secure Desktop capture/control remains a separate incomplete product capability and is not reclassified by this Rust decision.

## Phase-1 conclusion

Phase 1 demonstrates that the proposed Rust coordination core is deterministic, property/stress-testable, has a narrow panic-contained ABI, has synthetic per-event overhead small enough to justify further measurement, and does not disturb the existing automated production regression baseline because it is still isolated. The deployment gate also found and closed a real bootstrap invocation issue rather than bypassing it.

The evidence is sufficient to proceed to **shadow-only integration** so that product, memory, real-network and long-soak metrics can be collected without changing production authority. It is not sufficient for any Rust authority transfer.

Decision: ADVANCE_TO_SHADOW_INTEGRATION