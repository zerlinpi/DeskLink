# DeskLink architecture

## Product target

DeskLink is designed for a Sunlogin / NetEase UU Remote-like experience: low interaction latency, easy device discovery, automatic NAT traversal, browser access, and native host performance.

## Chosen stack

| Layer | Choice | Why |
| --- | --- | --- |
| Windows host | C++20 + DXGI Desktop Duplication + D3D11 | Lowest practical capture overhead and direct access to GPU textures |
| Encode | Media Foundation hardware H.264 first | Browser-compatible, hardware accelerated on common Intel/NVIDIA/AMD systems |
| Realtime transport | WebRTC | Mature ICE/STUN/TURN, SRTP encryption, browser interoperability and recovery primitives |
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
  -> GPU BGRA -> NV12 conversion / scaling
  -> Media Foundation hardware H.264 encoder
  -> H.264 RTP packetizer
  -> RTP pacer / DTLS-SRTP / ICE
  -> browser hardware decoder
  -> video element / GPU compositor
```

Frames remain on the GPU through capture, color conversion and encoder input. A frame that is already stale should be dropped rather than queued. Pointer movement is carried on a separate unreliable/unordered DataChannel so stale pointer packets do not block newer movement.

## Network behavior

1. Host and controller register with the signaling service using device IDs.
2. Controller creates an authenticated WebRTC offer.
3. ICE attempts host/server-reflexive candidates first.
4. If direct UDP fails, TURN/UDP is the preferred relay path; TURN/TCP is a compatibility fallback. TURN/TLS is a planned final restrictive-network fallback.
5. Media never traverses the signaling service.
6. The browser samples RTT/loss/jitter/decode FPS/available bitrate every second.
7. Signaling WebSockets reconnect independently of a healthy established P2P session.
8. If the WebRTC path becomes `disconnected`, the controller first allows a brief natural recovery window; `failed` triggers an immediate ICE restart.
9. ICE restart re-negotiates the same authorized controller/session rather than creating a new remote-control session.

### Loss recovery and packet pacing

The H.264 media chain includes:

- RTP packetization.
- RTCP sender reports.
- NACK-based packet retransmission.
- PLI/FIR handling that immediately asks the hardware encoder for a fresh IDR.
- RTP pacing that smooths encoder output bursts over short intervals instead of emitting a complete complex frame as one network burst.

The pacer defaults to roughly 1.2x the configured video bitrate with a 5 ms send interval, leaving enough headroom to avoid creating its own long queue.

### Adaptive quality policy

Initial desktop profile on capable hardware:

- Up to 1920x1080 by default (configurable).
- Up to 60 FPS.
- 12 Mbps default target bitrate (configurable).
- H.264 low-latency mode.
- Short GOP with on-demand IDR recovery.

Congestion response is deliberately ordered to protect interaction latency:

1. Reduce encoder bitrate quickly based on loss, RTT, jitter and estimated incoming capacity.
2. If severe pressure persists, reduce effective encoded frame cadence stepwise (for a 60 FPS target: 60 -> 45 -> 30 -> 24 FPS).
3. Only after bitrate and FPS are already constrained does sustained severe congestion lower encode resolution through 1600x900 -> 1280x720 -> 960x540 limits.
4. Keep the input DataChannels independent and responsive throughout video degradation.
5. Recover bitrate/FPS/resolution more slowly than they degrade to prevent oscillation.
6. Rebuild the GPU/Media Foundation pipeline safely when the resolution tier or host display dimensions change; failed adaptive rebuilds attempt to restore the previous profile.

## Static desktop and first-frame behavior

DXGI may provide no fresh texture while the desktop is completely static. DeskLink retains the latest converted GPU NV12 frame at a low idle refresh cadence. A new connection, PLI/FIR or severe-loss recovery can re-encode that cached frame as an IDR without waiting for desktop motion.

This avoids a common remote-desktop failure mode where a static login/application screen appears black until something changes visually.

## Multi-monitor input mapping

The selected DXGI output exposes its desktop rectangle, including negative virtual-desktop coordinates. Browser coordinates are normalized against the actual rendered video content (excluding `object-fit: contain` letterboxing), then Windows `SendInput` uses virtual-desktop absolute coordinates.

This keeps pointer input aligned when a secondary display is left/above the primary display or when monitors are rearranged.

## Windows scheduling

The interactive Windows Agent uses conservative latency-oriented scheduling:

- `ABOVE_NORMAL_PRIORITY_CLASS`, not realtime priority.
- MMCSS `Capture` task with high MMCSS thread priority when available.
- 1 ms multimedia timer period to reduce capture/pacing wake-up variance.

This can be disabled with `DESKLINK_PERFORMANCE_TUNING=0` for compatibility comparisons. The project intentionally avoids `REALTIME_PRIORITY_CLASS`, which can starve important Windows threads and make the system less reliable.

## Windows host milestones

### M1 — visible desktop control

Implemented core:

- `IDXGIOutput1::DuplicateOutput` capture and `DXGI_ERROR_ACCESS_LOST` recovery.
- D3D11 GPU texture pipeline.
- Media Foundation hardware H.264 MFT.
- Native WebRTC peer connection.
- Browser H.264 playback.
- Pointer + keyboard `SendInput`.
- Multi-monitor coordinate mapping.
- Access-code authorization.

### M2 — transport/performance reliability

Implemented core:

- Telemetry-driven bitrate/FPS/resolution adaptation.
- NACK + PLI/FIR video recovery.
- RTP pacing.
- Cached static-frame IDR recovery.
- Signaling reconnect.
- Browser ICE restart and same-session re-negotiation.
- Live browser network diagnostics.

Remaining M2 product features include clipboard/file transfer and audio.

### M3 — unattended operation

Use a two-process model:

- Windows Service running as LocalSystem: device identity, persistent network session, updates, privileged coordination.
- Per-user Session Agent: captures the interactive session and injects normal input.

Secure Desktop/UAC and login-screen control need additional privileged handling and must not be faked by weakening Windows security settings.

## Security baseline before public exposure

The current access-code model is still development-oriented. Before public deployment:

- Require authenticated short-lived signaling tokens.
- Bind devices to accounts and require explicit authorization / strong access credentials.
- Use HTTPS/WSS and valid certificates.
- Use TURN REST temporary credentials instead of static TURN passwords.
- Restrict WebSocket origins.
- Rate-limit authentication/session attempts across connections/IPs, not only individual sockets.
- Maintain connection/audit logs without recording keyboard content or raw access credentials.
- Rotate server secrets and deploy regional relay infrastructure.

## What “smooth” means

No remote-desktop product can guarantee zero stutter on every network. DeskLink's measurable targets are:

- LAN capture-to-display: ideally <100 ms.
- Good regional WAN: typically <180 ms.
- Pointer/control messages not blocked behind video queues.
- No unbounded video frame/RTP queue.
- Fast keyframe recovery after loss.
- Graceful bitrate/FPS/resolution degradation under congestion.
- Automatic recovery from temporary signaling or ICE path changes where the underlying network permits it.

These targets should be validated with the repeatable matrix in `docs/NETWORK_TESTING.md`, not subjective visual checks alone.
