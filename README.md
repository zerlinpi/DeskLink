# DeskLink

DeskLink is a low-latency remote desktop project targeting a Sunlogin / NetEase UU Remote-like experience with a browser-first controller and native host agents.

## Architecture direction

- **Controller:** Web (React + TypeScript) first, so macOS/Windows/mobile browsers can control a remote device without installing a controller.
- **Windows host agent:** Native C++20 using DXGI Desktop Duplication, D3D11 GPU scaling/color conversion, Media Foundation hardware H.264, and Win32 `SendInput`.
- **Windows supervisor:** Native Windows Service keeps a per-user Agent running in the active logged-in session and owns durable machine identity.
- **Realtime transport:** WebRTC (ICE/STUN/TURN, DTLS-SRTP, RTP, DataChannels) with P2P preferred and TURN relay fallback.
- **Signaling:** Go WebSocket/HTTP service. It coordinates sessions and issues short-lived signaling/relay credentials; media does not flow through signaling.
- **Relay:** coturn with UDP/TCP/TLS support and TURN REST temporary credentials.
- **Future native controllers:** Swift/Kotlin/native desktop controllers can reuse the protocol and signaling layer.

> No remote-control product can guarantee zero stutter on every network. DeskLink is designed to prioritize input responsiveness and low media latency, reducing bitrate/FPS/resolution before allowing long media queues to build.

## Current status

Implemented on `main`:

- Device registration and WebSocket offer/answer/ICE signaling.
- Long-lived unattended Windows device credential -> short-lived signaling token -> temporary TURN credential chain.
- Optional HMAC-based short-lived signaling registration tokens bound to an exact device ID.
- Per-device revocation through an inline list or administrator-owned file; revocation blocks new signal/TURN credentials, new WebSocket registration, and disconnects existing signaling sessions on the keepalive cycle.
- Signaling Ping/Pong keepalive, graceful server shutdown and automatic reconnect with exponential backoff.
- Signaling message-size/type/session validation, per-connection throttling, cross-connection/IP handshake limiting and authentication-failure backoff.
- Optional protected signaling metrics endpoint.
- STUN plus TURN UDP/TCP/TLS fallback, with P2P preferred.
- Authenticated TURN REST temporary-credential endpoint; responses are short-lived and `no-store`.
- Windows runtime Signal Token renewal plus TURN credential retrieval, with optional fail-closed modes.
- Browser runtime TURN credential retrieval before initial connection and refresh before ICE restart, with optional fail-closed mode.
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
- Windows scheduling tuning: above-normal process priority, MMCSS capture priority and 1 ms timer period (can be disabled).
- `desklink-service.exe` starts/restarts the Agent in the active logged-in Windows user session, reacts to session changes and uses crash-loop exponential backoff.
- Graceful Service -> Agent shutdown: remote keys/buttons are released first, normal WebRTC/GPU/media cleanup gets a five-second window, then forced termination is used only as fallback.
- Machine-scope DPAPI storage for the durable Windows device credential under `%ProgramData%\DeskLink`, with SYSTEM/Administrators-only file ACL.
- Service-owned local authentication broker: the DPAPI-protected long-lived device credential stays inside LocalSystem; the user-session Agent receives only short-lived Signal Tokens.
- Local authentication Pipe hardening: local-only Named Pipe, synchronous first-instance creation, exact Agent user-SID ACL and exact Agent PID verification.
- Service-side short Signal Token cache with a 90-second expiry safety margin to reduce unnecessary backend exchanges and improve reconnect resilience.
- Windows CI runtime validation for DPAPI storage and the local auth broker, including same-user/wrong-PID rejection.
- CI for signaling, browser and Windows native builds, including stale-run cancellation, native dependency caching and downloadable Windows Agent/Service artifacts.

Still required before calling the project production-ready:

- Real Windows GPU/runtime validation across representative Intel / NVIDIA / AMD hardware and drivers.
- Browser/H.264 compatibility testing across Chrome/Edge/Safari and mobile browsers.
- Production HTTPS/WSS deployment and certificate/reverse-proxy validation.
- A real account/device registry that authenticates users/controllers, stores independent per-device keys/hashes, supports credential rotation/ownership/audit history and issues short-lived browser registration tokens at runtime.
- Regional TURN deployment, relay health/routing policy and real WAN latency measurements.
- Move the remaining unattended access-code secret out of plaintext machine environment configuration or replace it with the future account/session authorization model.
- Windows logon-screen and UAC Secure Desktop capture/control through a tightly scoped privileged broker. The current Service covers logged-in unattended persistence only.
- Installer/code-signing hardening.
- Audio, clipboard/file transfer and native mobile UX.

## Repository layout

```text
apps/
  signal/         Go signaling + temporary credential service
  web/            Browser controller
  windows-agent/  Native Windows host + Windows Service
infra/
  coturn/         TURN configuration
  docker-compose.yml
packages/
  protocol/       Signaling and control protocol docs/types
docs/
  ARCHITECTURE.md
  DEVICE_AUTH.md
  NETWORK_TESTING.md
  PRODUCTION_NETWORK.md
  WINDOWS_SERVICE.md
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

The Release directory contains both `desklink-agent.exe` and `desklink-service.exe`. Successful GitHub Actions Windows jobs also upload both files as a `desklink-windows-<commit>` artifact.

Configure the host before starting it directly in development mode:

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

The static TURN username/password above are development fallback values. Production should use the short-lived device/signal/TURN flow documented in `docs/DEVICE_AUTH.md` and `docs/PRODUCTION_NETWORK.md`.

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

To install logged-in unattended persistence from an elevated shell:

```powershell
.\build\windows-agent\Release\desklink-service.exe --install
```

For unattended production-style identity, provision a `dc1...` device credential and store it with DPAPI:

```powershell
.\build\windows-agent\Release\desklink-service.exe --store-device-credential
Restart-Service DeskLink
```

In DPAPI mode, the long-lived credential remains inside the LocalSystem Service. The Service-owned local broker gives the user-session Agent only short-lived signaling material.

See `docs/WINDOWS_SERVICE.md` and `docs/DEVICE_AUTH.md` for limitations, migration and uninstall instructions.

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

For production-style runtime relay credentials, configure `VITE_TURN_CREDENTIALS_URL` and a short-lived signaling registration token; see `docs/PRODUCTION_NETWORK.md` and `apps/web/.env.restrictive.example`.

## Public/WAN deployment notes

Do not rely on the development defaults for a public deployment. In particular:

- Deploy your own STUN/TURN service close to the users. For China-focused use, do not depend on a public Google STUN endpoint.
- Prefer Direct P2P, then TURN/UDP. Use TURN/TCP or TURN/TLS only as compatibility fallbacks because they generally add more latency/jitter.
- Use multiple regional relay nodes once users are geographically distributed; a single relay cannot provide low latency everywhere.
- Use HTTPS/WSS and valid public certificates.
- Configure coturn `external-ip` correctly when the relay is behind NAT.
- Enable TURN REST temporary credentials and keep the coturn shared secret server-side only.
- Restrict signaling origins, require short-lived registration tokens, maintain a per-device revocation source, rate-limit authentication/session creation and keep audit metadata without logging keyboard content or access codes.

## Recommended validation order

1. Same Windows machine / browser, then same LAN, with firewall rules verified.
2. Two different LANs using Direct P2P where possible.
3. Force TURN/UDP relay and verify the diagnostics HUD reports `TURN relay`.
4. Test TURN/TCP and TURN/TLS separately as restrictive-network fallbacks.
5. Enable runtime TURN credentials and verify a newly issued credential is used for initial connection and ICE restart.
6. Add controlled loss/latency/bandwidth limits and verify quality drops before control responsiveness degrades.
7. Switch network paths during a session and verify signaling reconnect + ICE restart recover without manual reconnect.
8. Install the Windows Service and test logon, logout, fast-user/session changes, graceful Agent shutdown and crash recovery.
9. Provision the DPAPI device credential, remove the old plaintext machine credential and verify unattended reconnect after reboot/login.
10. Confirm the user-session Agent has no `DESKLINK_DEVICE_CREDENTIAL` in DPAPI/Service mode and still refreshes short-lived Signal Tokens through the local broker.
11. Revoke a test device and verify token issuance stops and any existing signaling WebSocket is disconnected within the keepalive interval.
12. Test Intel, NVIDIA and AMD hardware encoders independently.

See `docs/NETWORK_TESTING.md` for a repeatable weak-network test matrix.

## Milestones

1. **M0 — complete:** signaling, browser UI, WebRTC negotiation, input protocol, TURN fallback.
2. **M1 — implemented, runtime validation ongoing:** Windows GPU capture/encode, browser playback, mouse/keyboard input, access-code authorization.
3. **M2 — performance core implemented:** adaptive bitrate/FPS/resolution, PLI/NACK recovery, RTP pacing, reconnect/ICE restart, multi-monitor and diagnostics.
4. **M3 — in progress:** logged-in unattended Windows Service, short-lived credential chain, per-device revocation, graceful Agent lifecycle, DPAPI protected device identity and Service-owned PID/SID-bound short-token broker are implemented. Independent per-device key rotation, account/browser auth, protected access-code storage, Secure Desktop broker, clipboard/file transfer/audio, macOS and native mobile clients remain.

See `docs/ARCHITECTURE.md` for the detailed technical decisions.
