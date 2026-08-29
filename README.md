# DeskLink

DeskLink is a low-latency remote desktop project targeting a Sunlogin / NetEase UU Remote-like experience with a browser-first controller and native host agents.

## Architecture direction

- **Controller:** Web (React + TypeScript) first, so macOS/Windows/mobile browsers can control a remote device without installing a controller.
- **Windows host agent:** Native C++20 using DXGI Desktop Duplication, D3D11 GPU scaling/color conversion, Media Foundation hardware H.264, and Win32 `SendInput`.
- **Windows supervisor:** Native Windows Service keeps a per-user Agent running in the active logged-in session and owns durable machine identity/secrets.
- **Realtime transport:** WebRTC (ICE/STUN/TURN, DTLS-SRTP, RTP, DataChannels) with P2P preferred and TURN relay fallback.
- **Signaling:** Go WebSocket/HTTP service. It coordinates sessions and issues short-lived signaling/relay credentials; media does not flow through signaling.
- **Relay:** coturn with UDP/TCP/TLS support and TURN REST temporary credentials.
- **Future native controllers:** Swift/Kotlin/native desktop controllers can reuse the protocol and signaling layer.

> No remote-control product can guarantee zero stutter on every network. DeskLink is designed to prioritize input responsiveness and low media latency, reducing bitrate/FPS/resolution before allowing long media queues to build.

## Current status

Implemented on `main`:

- Device registration and WebSocket offer/answer/ICE signaling.
- Long-lived unattended Windows device credential -> short-lived signaling token -> temporary TURN credential chain.
- Independent per-device `dc2` credential registry with SHA-256-only server storage and hot single-device rotation; legacy deterministic `dc1` migration support remains.
- Runtime browser controller registry with hashed `ck1` keys, per-controller allowed-device lists, target-scoped short-lived `ct1` sessions and fail-closed registry loading.
- Runtime controller tokens remain in browser memory and are carried outside the WebSocket URL using an auth-bearing subprotocol request while the server negotiates only fixed `desklink-v1`.
- Signaling hub enforcement that a controller token may signal only to its authorized target, plus browser-side validation of target host/session for auth, answer and ICE messages.
- Per-device revocation through an inline list or administrator-owned file; revocation blocks new signal/TURN credentials, host/target-scoped controller registration, new signaling toward the target and disconnects existing signaling sockets on keepalive recheck.
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
- One-time Access Code challenge/proof before the initial WebRTC offer: 256-bit host nonce, HMAC-SHA256 proof bound to controller/host/session, 15-second challenge and one-off initial-offer authorization. The reusable Access Code is no longer sent in the offer.
- WebRTC diagnostics HUD showing Direct P2P/TURN route, protocol, RTT, loss, jitter, decode FPS and estimated available bitrate.
- Windows scheduling tuning: above-normal process priority, MMCSS capture priority and 1 ms timer period (can be disabled).
- `desklink-service.exe` starts/restarts the Agent in the active logged-in Windows user session, reacts to session changes and uses crash-loop exponential backoff.
- Graceful Service -> Agent shutdown: remote keys/buttons are released first, normal WebRTC/GPU/media cleanup gets a five-second window, then forced termination is used only as fallback.
- Machine-scope DPAPI storage for both the durable Windows device credential and unattended Access Code under `%ProgramData%\DeskLink`, with SYSTEM/Administrators-only ACL.
- Service-owned local authentication broker: the DPAPI-protected durable device credential stays inside LocalSystem; the user-session Agent receives only short-lived Signal Tokens.
- Independent broker capability for the DPAPI Access Code; protected mode removes `DESKLINK_ACCESS_CODE` from the Agent environment and fails closed if the broker cannot supply it.
- Local authentication Pipe hardening: local-only Named Pipe, synchronous first-instance creation, exact Agent user-SID ACL and exact Agent PID verification.
- Service-side short Signal Token cache with a 90-second expiry safety margin to reduce unnecessary backend exchanges and improve reconnect resilience.
- Windows CI runtime validation for DPAPI storage and the local auth broker, including same-user/wrong-PID rejection; CI covers both legacy `dc1` and registry `dc2` DPAPI provisioning.
- CI for signaling, browser and Windows native builds, provisioning-tool invariants, stale-run cancellation, native dependency caching and downloadable Windows Agent/Service artifacts.
- Development Compose support for read-only device/controller/revocation registry mounts; local `infra/secrets/` and web `.env.local` files are gitignored.

Still required before calling the project production-ready:

- Real Windows GPU/runtime validation across representative Intel / NVIDIA / AMD hardware and driver versions.
- Browser/H.264 compatibility testing across Chrome/Edge/Safari and representative mobile browsers/devices.
- A real public HTTPS/WSS deployment with reverse-proxy certificate, WebSocket, CORS, sensitive-log filtering and failure-mode validation.
- Replace/augment the current administrator-managed file registries with a durable account/device service if the product needs multi-user ownership transfer, recovery, role administration and audit history.
- If weak human PIN/password Access Codes must be supported, replace the current HMAC challenge with a PAKE/OPAQUE-style protocol; until then production Access Codes should be long random secrets.
- Regional TURN deployment, relay health/routing policy, capacity monitoring and real WAN latency/loss measurements.
- Windows logon-screen and UAC Secure Desktop capture/control through a tightly scoped privileged broker. The current Service covers logged-in unattended persistence only.
- Installer/update/code-signing hardening and release provenance.
- Audio, clipboard/file transfer and native mobile UX.

## Repository layout

```text
apps/
  signal/         Go signaling + short-lived credential service
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
  SIGNAL_AUTH.md
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

The Compose file also has an optional read-only `./secrets:/run/desklink-secrets` mount for production-style registry testing. `infra/secrets/` is ignored by Git; production should use a real secret/config delivery mechanism.

### 2. Build the Windows host

From a Visual Studio developer shell with CMake and Git available:

```powershell
cmake -S apps/windows-agent -B build/windows-agent -A x64
cmake --build build/windows-agent --config Release --parallel
```

The Release directory contains `desklink-agent.exe` and `desklink-service.exe`. Successful GitHub Actions Windows jobs upload both as a `desklink-windows-<commit>` artifact.

Direct Agent development example:

```powershell
$env:DESKLINK_SIGNAL_URL = "ws://YOUR_SERVER:8080/ws"
$env:DESKLINK_DEVICE_ID = "office-pc"
$env:DESKLINK_ACCESS_CODE = "use-a-long-random-development-code"
$env:DESKLINK_STUN_URL = "stun:YOUR_SERVER:3478"
$env:DESKLINK_TURN_HOST = "YOUR_SERVER"
$env:DESKLINK_TURN_USERNAME = "desklink"
$env:DESKLINK_TURN_PASSWORD = "CHANGE_ME_NOW"

.\build\windows-agent\Release\desklink-agent.exe
```

Static TURN credentials and plaintext Access Code environment variables are development fallbacks, not the recommended unattended production configuration.

Optional video/performance tuning:

```powershell
$env:DESKLINK_FPS = "60"
$env:DESKLINK_BITRATE_BPS = "12000000"
$env:DESKLINK_MIN_BITRATE_BPS = "2000000"
$env:DESKLINK_MAX_WIDTH = "1920"
$env:DESKLINK_MAX_HEIGHT = "1080"
$env:DESKLINK_PACING_BPS = "14400000"
$env:DESKLINK_PERFORMANCE_TUNING = "1"
```

### 3. Install logged-in unattended persistence

```powershell
.\build\windows-agent\Release\desklink-service.exe --install
```

For production-style identity, first create a `dc2` server-registry credential using `tools/auth/rotate-device-registry-credential.go`, then store it on Windows:

```powershell
.\build\windows-agent\Release\desklink-service.exe --store-device-credential
```

Store a long random unattended Access Code separately:

```powershell
.\build\windows-agent\Release\desklink-service.exe --store-access-code
Restart-Service DeskLink
```

In protected mode both durable secrets are removed from the Agent environment. The LocalSystem broker keeps the device credential on the privileged side and supplies the Agent only short-lived signaling material plus the Access Code capability required for local host proof verification.

See `docs/WINDOWS_SERVICE.md` and `docs/DEVICE_AUTH.md` for migration and failure behavior.

### 4. Start the browser controller

```bash
cd apps/web
npm install
npm run dev
```

Simple local-development profile:

```dotenv
VITE_SIGNAL_URL=ws://YOUR_SERVER:8080/ws
VITE_STUN_URL=stun:YOUR_SERVER:3478
VITE_TURN_URL=turn:YOUR_SERVER:3478
VITE_TURN_USERNAME=desklink
VITE_TURN_PASSWORD=CHANGE_ME_NOW
```

Production-style browser profile:

```dotenv
VITE_SIGNAL_URL=wss://control.example.com/ws
VITE_CONTROLLER_SESSION_URL=https://control.example.com/api/v1/controller-session
VITE_CONTROLLER_AUTH_REQUIRED=1
VITE_STUN_URL=stun:turn.example.com:3478
VITE_TURN_URL=turn:turn.example.com:3478
VITE_TURN_TLS_URL=turns:turn.example.com:5349
VITE_TURN_CREDENTIALS_URL=https://control.example.com/api/v1/turn-credentials
VITE_TURN_RUNTIME_REQUIRED=1
```

The user enters controller account/key, target device ID and the target's Access Code at runtime. The controller key is exchanged for a target-scoped short session token. The Access Code is used locally to create a one-time challenge proof; it is not placed in the WebRTC offer.

See `docs/SIGNAL_AUTH.md`, `docs/PRODUCTION_NETWORK.md` and `apps/web/.env.restrictive.example`.

## Public/WAN deployment notes

Do not rely on development defaults publicly:

- deploy your own STUN/TURN close to users; for China-focused operation do not depend on a public Google STUN endpoint;
- prefer Direct P2P, then TURN/UDP, with TURN/TCP/TLS as compatibility fallbacks;
- deploy multiple relay regions once users are geographically distributed;
- use HTTPS/WSS and valid public certificates;
- configure coturn `external-ip` correctly behind NAT;
- use TURN REST temporary credentials and keep the shared secret server-side;
- disable allow-any-origin, restrict browser origins and trust forwarded-IP headers only behind a non-bypassable proxy;
- suppress sensitive Authorization/subprotocol/query credentials from reverse-proxy logs;
- use administrator-owned device/controller/revocation registries and short-lived runtime tokens;
- keep Access Codes high entropy with the current HMAC challenge protocol.

## Recommended validation order

1. Same Windows machine/browser, then same LAN, with firewall rules verified.
2. Two different LANs using Direct P2P where possible.
3. Force TURN/UDP relay and verify the HUD reports `TURN relay`.
4. Test TURN/TCP and TURN/TLS separately as restrictive-network fallbacks.
5. Enable runtime controller and TURN credentials; confirm no production controller bearer token appears in the WebSocket URL.
6. Verify the Access Code is absent from signaling offer payloads and wrong challenge proofs are rejected before PeerConnection creation.
7. Add controlled loss/latency/bandwidth limits and verify quality drops before control responsiveness degrades.
8. Switch network paths during a session and verify signaling reconnect + ICE restart recover without manual reconnect.
9. Install the Windows Service and test logon, logout, fast-user/session changes, graceful Agent shutdown and crash recovery.
10. Provision a `dc2` DPAPI device credential and protected Access Code; remove old plaintext machine variables and verify reconnect after reboot/login.
11. Confirm the user-session Agent has no durable device credential or protected Access Code environment variable in Service mode.
12. Revoke a test device and verify token issuance stops and existing signaling sockets close on keepalive recheck.
13. Test Intel, NVIDIA and AMD hardware encoders/drivers independently.
14. Repeat on current Chrome, Edge, Safari and representative mobile browsers.

See `docs/NETWORK_TESTING.md` for the weak-network test matrix.

## Milestones

1. **M0 — complete:** signaling, browser UI, WebRTC negotiation, input protocol, TURN fallback.
2. **M1 — implemented; hardware/browser validation ongoing:** Windows GPU capture/encode, browser playback, mouse/keyboard input and host Access Code authorization.
3. **M2 — performance core implemented:** adaptive bitrate/FPS/resolution, PLI/NACK recovery, RTP pacing, reconnect/ICE restart, multi-monitor and diagnostics.
4. **M3 — security/unattended core implemented; production validation ongoing:** logged-in Windows Service, graceful lifecycle, DPAPI device credential + Access Code, PID/SID-bound broker, independent `dc2` rotation, revocation, runtime controller registry/sessions, target scope, temporary TURN and one-time host Access Code proof are implemented. Remaining M3 production work is primarily real deployment/hardware/browser validation, durable multi-user account service requirements, PAKE for weak passwords, Secure Desktop, signing/installer and operational relay rollout.
5. **Later product scope:** clipboard/file transfer/audio, macOS host and native mobile/desktop controller UX.

See `docs/ARCHITECTURE.md` for detailed technical decisions.
