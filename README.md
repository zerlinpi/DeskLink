# DeskLink

DeskLink is a low-latency remote desktop project targeting a Sunlogin / NetEase UU Remote-like experience with a browser-first controller and native host agents.

## Architecture direction

- **Controller:** Web (React + TypeScript) first, so macOS/Windows/mobile browsers can control a remote device without installing a controller.
- **Windows host agent:** Native C++20 using DXGI Desktop Duplication, D3D11 GPU scaling/color conversion, Media Foundation hardware H.264, and Win32 `SendInput`.
- **Realtime transport:** WebRTC (ICE/STUN/TURN, DTLS-SRTP, RTP, DataChannels) with P2P preferred and TURN relay fallback.
- **Signaling:** Go WebSocket/HTTP service. It only coordinates sessions; media does not flow through signaling.
- **Relay:** coturn.
- **Future native controllers:** Tauri/Swift/Kotlin can reuse the protocol and signaling layer.

> No remote-control product can guarantee zero stutter on every network. DeskLink is designed to prioritize input responsiveness and low media latency, degrading bitrate/resolution before allowing long queues to build.

## Current M1 status

Implemented on `main`:

- Device registration and WebSocket offer/answer/ICE signaling.
- Server Ping/Pong keepalive for long-lived device registrations.
- STUN plus TURN UDP/TCP fallback.
- Browser H.264-only receive negotiation and video rendering.
- Separate reliable control and unreliable pointer DataChannels to reduce head-of-line blocking.
- Windows DXGI Desktop Duplication capture.
- D3D11 Video Processor GPU scaling plus BGRA -> NV12 conversion.
- Media Foundation hardware H.264 encoder configured for low latency, CBR, no B-frames and a one-second GOP target.
- H.264 RTP packetization and WebRTC video track.
- Win32 mouse/keyboard injection.
- Per-device access code checked before a PeerConnection is created.
- CI for Go signaling, browser build and Windows C++ build.

Still to validate/finish before calling M1 production-ready:

- Real Windows GPU end-to-end runtime validation across Intel / NVIDIA / AMD hardware.
- Browser/H.264 profile compatibility matrix and fallback handling.
- Network telemetry-driven bitrate adaptation.
- Production WSS/TURN temporary credentials and brute-force protection.
- Windows Service + per-session Agent for unattended logon/UAC scenarios.

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
```

## Local development

### 1. Start signaling and TURN

The provided Compose file is intended for a Linux development host/server because coturn uses host networking.

```bash
cd infra
docker compose up
```

For LAN-only testing, TURN is not normally used when WebRTC can establish a direct route. For internet testing, configure the coturn public IP/firewall first and replace the development TURN password in `infra/coturn/turnserver.conf`.

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
$env:DESKLINK_TURN_HOST = "YOUR_SERVER"
$env:DESKLINK_TURN_USERNAME = "desklink"
$env:DESKLINK_TURN_PASSWORD = "CHANGE_ME_NOW"

.\build\windows-agent\Release\desklink-agent.exe
```

Optional video tuning:

```powershell
$env:DESKLINK_FPS = "60"
$env:DESKLINK_BITRATE_BPS = "12000000"
$env:DESKLINK_MAX_WIDTH = "1920"
$env:DESKLINK_MAX_HEIGHT = "1080"
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

## Recommended validation order

1. Same Windows machine / browser, then same LAN, with firewall rules verified.
2. Two different LANs using P2P where possible.
3. Force TURN relay to verify the fallback path independently.
4. Add packet loss / latency / bandwidth shaping and verify that input remains responsive.
5. Test Intel, NVIDIA and AMD hardware encoders independently.

Do not expose the development configuration directly to the internet. Production deployment must use HTTPS/WSS, strong per-device credentials or challenge authentication, temporary TURN credentials, rate limiting, origin restrictions and audit logging.

## Milestones

1. **M0:** signaling, browser UI, WebRTC negotiation, input protocol, TURN fallback.
2. **M1:** Windows GPU capture/encode, browser playback, mouse/keyboard input, access-code authorization.
3. **M2:** adaptive bitrate/FPS/resolution, reconnect, multi-monitor, clipboard/file transfer.
4. **M3:** macOS host, Android/iOS controllers, unattended Windows service/session-agent architecture and hardened account/device authentication.

See `docs/ARCHITECTURE.md` for the detailed technical decisions.
