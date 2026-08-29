# DeskLink production network deployment

This document focuses on the network path required for a Sunlogin/UU Remote-like experience. The priority order is always:

1. direct ICE/P2P when possible;
2. TURN over UDP when relay is required;
3. TURN over TCP for restrictive NAT/firewalls;
4. TURN over TLS (`turns:`) as the final compatibility path.

The media path must never be proxied through the signaling WebSocket service.

## Recommended public topology

Use separate DNS names even if services initially share one VM:

- `control.example.com` -> HTTPS/WSS signaling reverse proxy.
- `turn.example.com` -> coturn public address.

For production, place TURN nodes geographically close to users. A single distant relay can make a correctly implemented remote desktop feel slow because every media/control packet detours through that relay.

## Ports

Recommended baseline:

- TCP 443: HTTPS/WSS signaling and runtime credential API.
- UDP/TCP 3478: STUN/TURN.
- TCP 5349: TURN/TLS (`turns:`).
- UDP 49160-49299: TURN relay allocation range in the supplied TLS example.

A provider may also expose TURN/TLS on TCP 443 on a dedicated IP when enterprise/hotel networks only permit outbound 443. Do not place a normal HTTPS reverse proxy in front of TURN unless the proxy explicitly supports raw TCP/TLS forwarding; TURN is not HTTP.

## coturn

Start from `infra/coturn/turnserver.tls.example.conf` and replace all placeholders. Important requirements:

- use a real DNS hostname and publicly trusted certificate;
- set `external-ip=PUBLIC_IP/PRIVATE_IP` when the TURN node is behind 1:1 NAT;
- open the complete relay UDP range in the cloud security group and host firewall;
- use `use-auth-secret` + `static-auth-secret` only with a long random secret stored outside source control;
- mint short-lived TURN credentials in the application backend rather than distributing the shared secret;
- monitor relay bandwidth because remote desktop video can consume several Mbps per active session.

Validate the TLS listener before browser testing:

```bash
./tools/network/check-turn-tls.sh turn.example.com 5349
```

## Signaling registration security

The signaling server can require a short-lived HMAC registration token bound to an exact `deviceId` by setting:

```bash
DESKLINK_SIGNAL_AUTH_SECRET=<long-random-secret>
```

When enabled, `/ws` requires `?deviceId=...&auth=...`. The Windows Agent reads its token from `DESKLINK_SIGNAL_AUTH_TOKEN`. The browser controller supports a pre-provisioned `VITE_SIGNAL_DEVICE_ID` + `VITE_SIGNAL_AUTH_TOKEN` pair for controlled testing.

Do not treat a build-time Vite token as the final public-web authentication architecture. `VITE_*` values are bundled into client JavaScript. A production account/device service should authenticate the user, mint a short-lived controller registration token at runtime, and deliver it only to that browser session.

The signaling server also applies per-connection message limits plus cross-connection/IP handshake throttling. By default it trusts the TCP peer address only. If a trusted reverse proxy terminates HTTPS/WSS and rewrites client-IP headers, set:

```bash
DESKLINK_TRUST_PROXY_HEADERS=1
```

Only enable that setting when the signaling server is not directly reachable around the proxy; otherwise a client could forge forwarding headers.

An optional protected operational endpoint can be enabled with:

```bash
DESKLINK_METRICS_TOKEN=<random-bearer-token>
```

Then `GET /metricsz` with `Authorization: Bearer <token>` returns aggregate active/total connections, rate-limited handshakes, authentication failures and forwarded signaling-message counts. The endpoint returns 404 when no metrics token is configured.

## Runtime TURN credentials

DeskLink can issue coturn REST-compatible temporary credentials through the signaling service. Enable it only after signaling registration tokens are enabled:

```bash
DESKLINK_SIGNAL_AUTH_SECRET=<same-signal-HMAC-secret-used-for-registration>
DESKLINK_TURN_AUTH_SECRET=<same-secret-configured-as-coturn-static-auth-secret>
DESKLINK_TURN_CREDENTIAL_TTL=12h
```

`GET /api/v1/turn-credentials?deviceId=<device-id>` requires:

```http
Authorization: Bearer <valid-signal-registration-token-for-that-device-id>
```

The endpoint is absent unless both auth secrets are configured. Responses are `Cache-Control: no-store`. TURN credentials default to 12 hours and are capped at 24 hours.

The TURN shared secret must never be delivered to Windows Agents or browsers. Clients receive only the temporary username/password pair generated from that secret.

## Browser controller

Normal profile should prefer direct/UDP paths while keeping TLS available as a final relay candidate:

```dotenv
VITE_SIGNAL_URL=wss://control.example.com/ws
VITE_SIGNAL_DEVICE_ID=web-controller-01
VITE_SIGNAL_AUTH_TOKEN=SHORT_LIVED_CONTROLLER_TOKEN
VITE_STUN_URL=stun:turn.example.com:3478
VITE_TURN_URL=turn:turn.example.com:3478
VITE_TURN_TLS_URL=turns:turn.example.com:5349
VITE_TURN_CREDENTIALS_URL=https://control.example.com/api/v1/turn-credentials
VITE_TURN_RUNTIME_REQUIRED=1
```

When `VITE_TURN_CREDENTIALS_URL` is configured, the controller fetches fresh temporary TURN credentials before the initial PeerConnection and again before every ICE restart, then updates the PeerConnection ICE configuration before renegotiating. This prevents long-lived browser tabs from attempting recovery with expired relay credentials.

`VITE_TURN_RUNTIME_REQUIRED=1` makes credential-fetch failures fail closed instead of falling back to `VITE_TURN_USERNAME` / `VITE_TURN_PASSWORD`. Leave static credentials only for local development or controlled migration.

The controller registers TURN/UDP and TURN/TCP from `VITE_TURN_URL`, and adds `VITE_TURN_TLS_URL` when configured. For explicit relay validation, set:

```dotenv
VITE_ICE_TRANSPORT_POLICY=relay
```

Use `apps/web/.env.restrictive.example` as the reference test profile. In normal production operation leave the transport policy unset so direct ICE remains preferred.

## Windows host

The Windows host registers STUN plus TURN/UDP, TURN/TCP and TURN/TLS candidates through libdatachannel. TLS defaults to port `5349`; set `DESKLINK_TURN_TLS_PORT=0` to disable the TLS candidate or set another port when the relay uses a custom listener.

Production runtime-credential example:

```powershell
$env:DESKLINK_SIGNAL_URL = "wss://control.example.com/ws"
$env:DESKLINK_DEVICE_ID = "win-office-01"
$env:DESKLINK_SIGNAL_AUTH_TOKEN = "SHORT_LIVED_DEVICE_TOKEN"
$env:DESKLINK_STUN_URL = "stun:turn.example.com:3478"
$env:DESKLINK_TURN_HOST = "turn.example.com"
$env:DESKLINK_TURN_PORT = "3478"
$env:DESKLINK_TURN_TLS_PORT = "5349"
$env:DESKLINK_TURN_CREDENTIALS_URL = "https://control.example.com/api/v1/turn-credentials"
$env:DESKLINK_TURN_RUNTIME_REQUIRED = "1"
```

For each new PeerConnection, the Agent requests a fresh temporary TURN username/password using its signaling registration token. With `DESKLINK_TURN_RUNTIME_REQUIRED=1`, a credential-fetch failure disables TURN for that new session instead of silently using configured static credentials. Direct/STUN ICE can still succeed when the network permits it.

The Windows Agent intentionally does not print authenticated signaling URLs, registration tokens or TURN passwords to normal logs.

## Windows service

`desklink-service.exe` is a LocalSystem session supervisor. It launches `desklink-agent.exe` into the active logged-in user session, restarts the Agent after unexpected exits, and reacts to Windows session changes. The capture/input process itself therefore remains in the interactive user session instead of running as LocalSystem.

This service currently provides logged-in unattended persistence. It does not yet claim Windows logon-screen or UAC Secure Desktop capture/control; those require a separate, tightly scoped privileged broker/session design.

## Regional relay policy

For low latency, route devices to the nearest healthy TURN region. A practical initial layout for East Asia could be:

- Japan/Korea region;
- North/East China-adjacent region where legally/operationally appropriate;
- South China/Hong Kong-adjacent region where legally/operationally appropriate;
- Southeast Asia region.

The exact hosting regions depend on the product's legal, network and user-distribution requirements. Measure real RTT instead of assuming geographic distance equals network distance.

## Acceptance targets

Use the HUD and `docs/NETWORK_TESTING.md` rather than subjective visual checks. For each route, record:

- selected route: Direct or Relay;
- relay protocol: UDP/TCP/TLS where observable;
- RTT, jitter and packet loss;
- decoded FPS;
- resolution tier and bitrate behavior;
- input responsiveness during congestion;
- time to recover after a network path change.

A relay path is acceptable only if it remains interactive under the target region's real RTT. If TURN/TCP/TLS is functional but consistently sluggish, the fix is usually relay placement/capacity rather than increasing video bitrate.
