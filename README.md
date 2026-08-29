# DeskLink

DeskLink is a low-latency remote desktop project targeting a Sunlogin / NetEase UU Remote-like experience with a browser-first controller and native host agents.

## Architecture direction

- **Controller:** Web (React + TypeScript) first, so macOS/Windows/mobile browsers can control a remote device without installing a controller.
- **Windows host agent:** Native C++20 using DXGI Desktop Duplication, D3D11 GPU scaling/color conversion, Media Foundation hardware H.264, and Win32 `SendInput`.
- **Realtime transport:** WebRTC (ICE/STUN/TURN, DTLS-SRTP, RTP, DataChannels) with P2P preferred and TURN relay fallback.
- **Signaling:** Go WebSocket/HTTP service. It only coordinates sessions; media does not flow through signaling.
- **Relay:** coturn.
- **Future native controllers:** Swift/Kotlin/native desktop controllers can reuse the protocol and signaling layer.

> No remote-control product can guarantee zero stutter on every network. DeskLink is designed to prioritize input responsiveness and low media latency, reducing bitrate/FPS/resolution before allowing long media queues to build.

## Current status

Implemented on `main`:

- Device registration and WebSocket offer/answer/ICE signaling.
- Signaling Ping/Pong keepalive plus automatic reconnect with exponential backoff.
- STUN plus TURN UDP/TCP fallback, with P2P preferred.
- Browser H.264 receive negotiation and hardware-decoded video rendering where supported.
- Separate reliable `control` and unreliable/unordered `pointer` DataChannels to reduce head-of-line blocking.
- Windows DXGI Desktop Duplication capture.
- D3D11 Video Processor GPU scaling plus BGRA -> NV12 conversion.
- Media Foundation hardware H.264 configured for low latency, CBR, no B-frames and short GOPs.
- H.264 RTP packetization, NACK retransmission support, RTCP PLI/FIR keyframe recovery and RTP pacing.
- Cached latest GPU frame so a new connection or PLI can recover even when the desktop is completely static.
- Telemetry-driven adaptive bitrate, FPS and resolution degradation/recovery.
- Adaptive FPS tiers down to 45/30/24 FPS when congestion persists.
- Adaptive resolution tiers up to configured maximum, then 1600x900, 1280x720 and 960x540 under sustained severe congestion.
- Browser signaling reconnect plus WebRTC ICE restart after network-path changes.
- Multi-monitor capture and virtual-desktop mouse coordinate mapping.
- Letterbox-aware browser pointer mapping.
- Win32 mouse/keyboard injection with stuck-key/mouse-button release protection.
- Per-device access code checked before a new PeerConnection is created.
- WebRTC diagnostics HUD showing Direct P2P/TURN route, protocol, RTT, loss, jitter, decode FPS and estimated available bitrate.
- Signaling message-size/type/session validation, connection rate limiting and Go tests.
- Windows scheduling tuning: above-normal process priority, MMCSS capture priority and 1 ms timer period (can be disabled).
- CI for signaling, browser and Windows native builds, including stale-run cancellation and Windows native build caching.

Still required before calling the project production-ready:

- Real Windows GPU/runtime validation across representative Intel / NVIDIA / AMD hardware and drivers.
- Browser/H.264 compatibility testing across Chrome/Edge/Safari and mobile browsers.
- Production HTTPS/WSS deployment and certificate handling.
- Account/device authentication stronger than the current development access-code flow.
- TURN REST temporary credentials instead of static development credentials.
- Regional TURN deployment and real WAN latency measurements.
- Windows Service + per-session Agent for unattended logon/UAC/secure-desktop scenarios.
- Audio, clipboard/file transfer and native mobile UX.

## Repository layout

```text
apps/
  signal/         Go signaling service
  web/            Browser controller
  windows-agent/  Native Windows host
infra/
  coturn/         TURN configuration
  docker-compose.yml
packages/
  protocol/       Signaling and control protocol docs/types
docs/
  ARCHITECTURE.md
  NETWORK_TESTING.md
```

## Local development

### 1. Start signaling and TURN

The provided Compose file is intended for a Linux development host/server because coturn uses host networking.

```bash
cd infra
docker compose up
```

For LAN-only testing, TURN is normally unused when WebRTC can establish a direct route. For internet testing, configure the coturn public IP/firewall first and replace the development TURN password in `infra/coturn/turnserver.conf`.

Required server ports for the provided development config:

- TCP `8080`: signaling WebSocket/HTTP.
- UDP/TCP `3478`: TURN/STUN listener.
- UDP `49160-49200`: TURN relay ports.

### 2. Build the Windows host

From a Visual Studio developer shell with CMake and Git available:

```powershell
cmake -S apps/windows-agent -B build/windows-agent -A x64
cmake --build build/windows-agent --config Release --parallel
```

Configure the host before starting it:

```powershell
$env:DESKLINK_SIGNAL_URL = "ws://YOUR_SERVER:8080/ws"
$env:DESKLINK_DEVICE_ID = "office-pc"
$env:DESKLINK_ACCESS_CODE = "use-a-long-random-code"
$env:DESKLINK_STUN_URL = "stun:YOUR_SERVER:3478"
$env:DESKLINK_TURN_HOST = "YOUR_SERVER"
$env:DESKLINK_TURN_USERNAME = "desklink"
$env:DESKLINK_TURN_PASSWORD = "CHANGE_ME_NOW"

.\build\windows-agent\Release\desklink-agent.exe
```

Optional video/performance tuning:

```powershell
$env:DESKLINK_FPS = "60"
$env:DESKLINK_BITRATE_BPS = "12000000"
$env:DESKLINK_MIN_BITRATE_BPS = "2000000"
$env:DESKLINK_MAX_WIDTH = "1920"
$env:DESKLINK_MAX_HEIGHT = "1080"

# Optional RTP pacing override. Default is ~1.2x DESKLINK_BITRATE_BPS.
$env:DESKLINK_PACING_BPS = "14400000"

# Set to 0 only for troubleshooting scheduling/driver compatibility.
$env:DESKLINK_PERFORMANCE_TUNING = "1"
```

If `DESKLINK_ACCESS_CODE` is not set, the host remains registered but rejects every incoming remote-control offer.

### 3. Start the browser controller

```bash
cd apps/web
npm install
npm run dev
```

For a controller not running on the same machine as the signaling/TURN services, create `apps/web/.env.local`:

```dotenv
VITE_SIGNAL_URL=ws://YOUR_SERVER:8080/ws
VITE_STUN_URL=stun:YOUR_SERVER:3478
VITE_TURN_URL=turn:YOUR_SERVER:3478
VITE_TURN_USERNAME=desklink
VITE_TURN_PASSWORD=CHANGE_ME_NOW
```

Open the page, enter the Windows host's `DESKLINK_DEVICE_ID` and matching `DESKLINK_ACCESS_CODE`, then connect.

## Public/WAN deployment notes

Do not rely on the development defaults for a public deployment. In particular:

- Deploy your own STUN/TURN service close to the users. For China-focused use, do not depend on a public Google STUN endpoint.
- Prefer Direct P2P, then TURN/UDP. Use TURN/TCP or TURN/TLS only as compatibility fallbacks because they generally add more latency/jitter.
- Use multiple regional relay nodes once users are geographically distributed; a single relay cannot provide low latency everywhere.
- Use HTTPS/WSS and valid public certificates.
- Configure coturn `external-ip` correctly when the relay is behind NAT.
- Replace static TURN credentials with short-lived TURN REST credentials.
- Restrict signaling origins, rate-limit authentication/session creation and keep audit metadata without logging keyboard content or access codes.

## Recommended validation order

1. Same Windows machine / browser, then same LAN, with firewall rules verified.
2. Two different LANs using Direct P2P where possible.
3. Force TURN/UDP relay and verify the diagnostics HUD reports `TURN relay`.
4. Test TURN/TCP separately as a restrictive-network fallback.
5. Add controlled loss/latency/bandwidth limits and verify quality drops before control responsiveness degrades.
6. Switch network paths during a session and verify signaling reconnect + ICE restart recover without manual reconnect.
7. Test Intel, NVIDIA and AMD hardware encoders independently.

See `docs/NETWORK_TESTING.md` for a repeatable weak-network test matrix.

## Milestones

1. **M0 — complete:** signaling, browser UI, WebRTC negotiation, input protocol, TURN fallback.
2. **M1 — implemented, runtime validation ongoing:** Windows GPU capture/encode, browser playback, mouse/keyboard input, access-code authorization.
3. **M2 — performance core implemented:** adaptive bitrate/FPS/resolution, PLI/NACK recovery, RTP pacing, reconnect/ICE restart, multi-monitor and diagnostics. Clipboard/file transfer/audio remain.
4. **M3 — planned:** macOS host, Android/iOS controllers, unattended Windows service/session-agent architecture and hardened account/device authentication.

See `docs/ARCHITECTURE.md` for the detailed technical decisions.
