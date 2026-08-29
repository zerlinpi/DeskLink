# DeskLink architecture

## Product target

DeskLink is designed for a Sunlogin / NetEase UU Remote-like experience: low interaction latency, easy device discovery, automatic NAT traversal, browser access, and native host performance.

## Chosen stack

| Layer | Choice | Why |
| --- | --- | --- |
| Windows host | C++20 + DXGI Desktop Duplication + D3D11 | Lowest practical capture overhead and direct access to GPU textures |
| Encode | Media Foundation hardware H.264 first | Browser-compatible, hardware accelerated on common Intel/NVIDIA/AMD systems |
| Realtime transport | WebRTC | Mature ICE/STUN/TURN, SRTP encryption, browser interoperability, congestion control |
| Input | WebRTC DataChannel + Win32 `SendInput` | Separates control latency from video delivery |
| Signaling | Go + WebSocket | Small footprint, simple deployment, good concurrency |
| Relay | coturn | Battle-tested TURN fallback for restrictive NAT/CGNAT |
| Web controller | React + TypeScript + native WebRTC APIs | No controller install, works on macOS/Windows/mobile browsers |

H.265 is intentionally not the first codec for the browser path because browser/platform interoperability is less uniform than H.264. Native clients can negotiate HEVC later.

## Critical latency pipeline

```text
Desktop compositor
  -> DXGI Desktop Duplication
  -> D3D11 GPU texture
  -> hardware H.264 encoder
  -> WebRTC RTP / congestion control
  -> browser hardware decoder
  -> video element / GPU compositor
```

The implementation should keep frames on the GPU where possible and avoid CPU RGB copies. A frame that is already stale should be dropped rather than queued.

## Network behavior

1. Both peers register with the signaling service using a device ID and authenticated session token.
2. Controller creates a WebRTC offer.
3. ICE attempts host/server-reflexive candidates first.
4. If direct UDP fails, TURN relays UDP; TCP/TLS TURN is the final compatibility fallback.
5. Media never traverses the signaling service.
6. During a live session, RTT/loss/jitter/available outgoing bitrate are sampled every second.

### Adaptive quality policy

Initial desktop profile on capable hardware:

- 1920x1080
- 60 FPS
- 8-16 Mbps starting range
- H.264 low-latency mode
- 1-2 s keyframe interval

Congestion response:

1. Immediately lower encoder bitrate.
2. If loss/RTT remains bad, cap FPS at 45/30.
3. Then reduce render/capture resolution to 1600x900 or 1280x720.
4. Keep the input channel responsive even while video quality is reduced.
5. Recover slowly to prevent oscillation.

## Windows host milestones

### M1 — visible desktop control

- `IDXGIOutput1::DuplicateOutput`
- dirty/move rectangle awareness
- D3D11 texture pipeline
- Media Foundation hardware H.264 MFT
- WebRTC native peer connection
- pointer + keyboard `SendInput`
- browser controller playback

### M2 — unattended operation

Use a two-process model:

- Windows Service running as LocalSystem: device identity, persistent network session, updates, privileged coordination.
- Per-user Session Agent: captures the interactive session and injects normal input.

Secure Desktop/UAC and login-screen control need additional privileged handling and must not be faked by weakening Windows security settings.

## Security baseline before public exposure

The current scaffold is development-only. Before exposing it to the internet:

- Require authenticated short-lived signaling tokens.
- Bind devices to accounts and require explicit authorization / access password.
- Use TURN REST temporary credentials instead of static credentials.
- Restrict WebSocket origins.
- Rate-limit authentication and session requests.
- Maintain connection/audit logs without recording keyboard content.
- Rotate server secrets and use TLS (`wss:` / `turns:`) in production.

## What “smooth” means

No remote-desktop product can guarantee zero stutter on every network. DeskLink's measurable targets are:

- LAN capture-to-display: ideally <100 ms
- good WAN: typically <180 ms
- pointer/control messages not blocked behind video queues
- no unbounded frame queue
- graceful quality degradation under congestion

These targets should be enforced with telemetry and repeatable weak-network tests rather than subjective visual checks alone.
