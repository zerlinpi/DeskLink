# DeskLink architecture

## Product target

DeskLink is designed for a Sunlogin / NetEase UU Remote-like experience: low interaction latency, easy device discovery, automatic NAT traversal, browser access, native Windows host performance, unattended recovery and a productized controller instead of a protocol demo.

The current architecture is intentionally retained and evolved rather than replaced. The main path remains Windows C++20 + DXGI/D3D11/Media Foundation, WebRTC, a Go signaling/control plane and a React/TypeScript browser controller.

## Chosen stack

| Layer | Choice | Why |
| --- | --- | --- |
| Windows host | C++20 + DXGI Desktop Duplication + D3D11 | Lowest practical capture overhead and direct access to GPU textures |
| Encode | Media Foundation hardware H.264 first | Browser-compatible, hardware accelerated on common Intel/NVIDIA/AMD systems |
| Realtime transport | WebRTC | Mature ICE/STUN/TURN, SRTP encryption, browser interoperability and recovery primitives |
| Input | WebRTC DataChannel + Win32 `SendInput` | Separates control latency from video delivery |
| Signaling | Go + WebSocket/HTTP | Small footprint, simple deployment, good concurrency |
| Relay | coturn | Battle-tested TURN fallback for restrictive NAT/CGNAT |
| Web controller | React + TypeScript + native WebRTC APIs | No controller install and direct access to browser WebRTC telemetry/recovery APIs |

H.265 is intentionally not the first codec for the browser path because browser/platform interoperability is less uniform than H.264. Native clients can negotiate HEVC or newer codecs later without replacing the existing browser path.

## Critical latency pipeline

```text
Desktop compositor
  -> DXGI Desktop Duplication
  -> D3D11 GPU texture
  -> GPU BGRA -> NV12 conversion / scaling
  -> Media Foundation hardware H.264 encoder
  -> H.264 RTP packetizer
  -> RTP pacer / DTLS-SRTP / ICE
  -> browser hardware decoder
  -> video element / GPU compositor
```

Frames remain on the GPU through capture, color conversion and encoder input. DeskLink does not intentionally read desktop pixels back to CPU memory in the normal video path. A stale frame should be dropped rather than queued.

Pointer movement is carried on a separate unreliable/unordered DataChannel and uses latest-wins/backpressure behavior so obsolete movement does not block newer coordinates. Mouse buttons and keyboard events remain reliable and ordered.

## Network behavior

1. Host and controller register with the signaling service using scoped device/controller identities.
2. Controller completes Access Code challenge/proof and creates an authenticated WebRTC offer.
3. ICE attempts direct host/server-reflexive candidates first.
4. If direct UDP fails, TURN/UDP is preferred; TURN/TCP is a compatibility fallback and TURN/TLS is available for restrictive deployments when configured.
5. Media and input do not traverse the signaling service.
6. The browser samples RTT, loss, jitter, decode FPS and available bitrate every second.
7. Signaling WebSockets reconnect independently of a healthy established P2P session.
8. If the WebRTC path becomes `disconnected`, the controller allows a short natural recovery window and then performs ICE restart; `failed` requests recovery immediately.
9. ICE restart re-negotiates the same authorized controller/session instead of asking the user to enter the device/access code again.

A future LAN Direct mode is intended to add local discovery and serverless negotiation before the public signaling path. It is **not implemented yet** and must not duplicate the existing WebRTC media/control stack; LAN Direct should feed the same peer/session abstraction.

### Loss recovery and packet pacing

The H.264 media chain includes:

- RTP packetization;
- RTCP sender reports;
- NACK-based packet retransmission;
- PLI/FIR handling that requests a fresh hardware-encoder IDR;
- RTP pacing that smooths encoder bursts rather than emitting an entire complex frame as one network burst.

Pacing budget and interval scale with the configured media rate/refresh target so the pacer does not become a hidden bottleneck for 90/120/144 FPS sessions.

### Adaptive quality policy

The host supports a configured target of **15–144 FPS**. `60 FPS` remains the compatibility-oriented default; 90/120/144 are opt-in and depend on source refresh rate, GPU encoder capability, network capacity and browser decode/render performance.

High-refresh adaptive tiers are:

```text
144 -> 120 -> 90 -> 60 -> 45 -> 30 -> 24 -> 15
```

If the GPU conversion/Media Foundation encoder cannot initialize at a requested high refresh rate, startup retries lower high-refresh tiers rather than immediately disabling video.

Congestion response deliberately protects interaction latency:

1. Reduce encoder bitrate quickly based on loss, RTT, jitter and estimated incoming capacity.
2. If pressure persists, reduce effective encoded frame cadence through the stable FPS ladder.
3. Only after bitrate and FPS are constrained does sustained congestion lower encode resolution.
4. Keep input DataChannels independent and responsive throughout video degradation.
5. Recover bitrate/FPS/resolution more slowly than they degrade to avoid oscillation.
6. Rebuild the GPU/Media Foundation pipeline safely when resolution tier or display dimensions change; failed rebuilds attempt to restore the previous working profile.

The current general-purpose policy is desktop-oriented. A separately tuned game/interaction mode is still future work; do not treat high-refresh support itself as a completed game mode.

## Desktop transition and first-frame recovery

DXGI Desktop Duplication can be invalidated by lock/unlock, Fast User Switching, display-mode changes and some desktop transitions. If `DXGI_ERROR_ACCESS_LOST` occurs and Windows temporarily refuses a new duplication object, DeskLink keeps the D3D11 device/adapter and retries duplication creation with bounded backoff. Returning to the normal interactive desktop therefore no longer requires manually restarting Agent/Service merely because one immediate recreation attempt failed.

This mechanism does **not** mean DeskLink can capture/control the UAC Secure Desktop or Windows sign-in desktop. It restores capture when the normal interactive desktop becomes accessible again.

DXGI can also provide no fresh texture while the desktop is completely static. DeskLink retains the latest converted GPU NV12 frame at a low idle refresh cadence. A new connection, PLI/FIR or recovery request can re-encode that cached frame as an IDR without waiting for desktop motion.

## Input authority and fail-safe cleanup

The browser sends `release-all` on blur/hidden state and on deliberate disconnect. The Windows host additionally treats transport loss and input-channel closure as authority-loss boundaries.

Synthetic key/button state is tracked process-wide. Key/button injection and pressed-state bookkeeping are serialized so a disconnect cannot race between `SendInput(KEYDOWN)` and bookkeeping. `ReleaseAllInjectedInput()` removes a tracked state only after its `KEYUP`/`MOUSEUP` is successfully injected; a temporary UIPI/desktop failure remains tracked for a later cleanup attempt. High-frequency pointer movement is not placed behind this mutex.

This design reduces stuck Ctrl/Alt/Shift/Win or mouse buttons after network transitions without adding locking to the pointer-move hot path.

## Multi-monitor and DPI mapping

The selected DXGI output exposes its desktop rectangle, including negative virtual-desktop coordinates. Browser coordinates are normalized against the actual rendered video content (excluding `object-fit: contain` letterboxing), then Windows `SendInput` maps into virtual-desktop absolute coordinates.

Monitor enumeration/switching is session-aware and attempts rollback if a selected output cannot rebuild its video pipeline. Mixed-DPI and dynamic hot-plug behavior still require broader real-hardware validation; the coordinate architecture intentionally uses physical desktop/output geometry rather than assuming a single 100% scaled primary monitor.

## Windows scheduling

The interactive Windows Agent uses conservative latency-oriented scheduling:

- `ABOVE_NORMAL_PRIORITY_CLASS`, not realtime priority;
- MMCSS `Capture` task with high MMCSS thread priority when available;
- 1 ms multimedia timer period to reduce capture/pacing wake-up variance.

This can be disabled with `DESKLINK_PERFORMANCE_TUNING=0` for compatibility comparisons. The project intentionally avoids `REALTIME_PRIORITY_CLASS`, which can starve important Windows threads and make the machine less reliable.

## Windows Service / Session Agent model

DeskLink uses a two-process trust model:

- **Windows Service (LocalSystem):** machine identity, protected secrets, authentication broker, active-session supervision and privileged coordination.
- **Per-user Session Agent:** DXGI capture, D3D11/MF media work, WebRTC session and normal-desktop `SendInput`.

The Service prefers a usable active console session; if no active console user exists it enumerates Terminal Services sessions and can select a genuine `WTSActive` RDP session. Session change, logoff/login, Agent crash and Service restart cause the old Agent to be cleaned up and the appropriate user-session Agent to be relaunched with crash-loop backoff.

### Secure Desktop / sign-in boundary

Windows sign-in UI and UAC Secure Desktop are outside the normal Session Agent trust boundary. DeskLink must not implement them by disabling UAC, changing consent policy, or moving the complete network/media stack into LocalSystem.

The intended future design is a **minimal privileged desktop broker** with authenticated local IPC and narrowly scoped operations for protected desktop capture/input/system actions. Until that component is implemented and reviewed, sign-in/UAC control must be reported as unsupported rather than partially simulated.

## Security baseline

Implemented security foundations include scoped Signal/Controller tokens, per-device credentials, DPAPI-protected unattended secrets, Access Code HMAC challenge/proof, device revocation, TURN temporary credentials, origin restrictions/rate limiting in production configuration, and DTLS/SRTP from WebRTC.

Production deployments should still:

- require HTTPS/WSS with valid certificates;
- use TURN REST temporary credentials instead of static passwords;
- restrict WebSocket/browser origins;
- rate-limit authentication/session attempts across connection/IP/account boundaries;
- avoid logging raw Access Codes, durable device credentials, controller keys or auth subprotocol material;
- rotate server secrets and deploy relay capacity near users;
- treat release code signing and update verification as part of the supply-chain security boundary.

## Test strategy

Normal Windows CI builds the complete host and now permanently runs:

- H.264 Annex-B normalization smoke;
- high-refresh/adaptive video policy smoke;
- active Windows/RDP session-selection smoke;
- Access Code proof vector;
- DPAPI protected-secret smoke;
- Service authentication broker smoke;
- portable runtime dependency checks.

Release builds run the same policy/session smokes in addition to packaging/signing hooks.

CI cannot replace real GPU/session/network testing. Required real-machine soak/matrix work remains Windows 10/11, Intel/NVIDIA/AMD, mixed GPU laptops, LAN/IPv6/NAT/CGNAT/hotspot/corporate networks and 1h/8h/24h/72h resource-leak tests.

## Current major gaps

The following are intentionally still marked incomplete:

- WASAPI system audio / microphone transport;
- serverless LAN Direct discovery/negotiation;
- UAC Secure Desktop / Windows sign-in control;
- explicit NVENC/oneVPL/AMF backend selection beyond Media Foundation hardware MFT discovery;
- virtual display / privacy screen / virtual HID-gamepad stack;
- automatic signed update + rollback pipeline;
- broad physical-machine 72-hour soak and browser/device certification matrix.

## What “smooth” means

No remote-desktop product can guarantee zero stutter on every network. DeskLink's measurable targets are:

- LAN capture-to-display: ideally <100 ms on suitable hardware;
- good regional WAN: typically <180 ms where routing permits;
- pointer/control messages never blocked behind video queues;
- no unbounded video frame/RTP queue;
- fast keyframe recovery after loss;
- graceful bitrate/FPS/resolution degradation under congestion;
- automatic recovery from signaling, ICE path and normal desktop transitions where Windows/network conditions permit.

These targets must be validated with the repeatable matrix in `docs/NETWORK_TESTING.md`, not only subjective visual checks.
