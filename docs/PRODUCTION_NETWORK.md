# DeskLink production network deployment

DeskLink's preferred media/control path is:

1. direct ICE/P2P;
2. TURN/UDP when relay is required;
3. TURN/TCP for restrictive networks;
4. TURN/TLS (`turns:`) as the final compatibility path.

The signaling service coordinates sessions and credentials only. Remote desktop media must not be proxied through the signaling WebSocket service.

## Recommended public topology

Use separate DNS names even when services initially share infrastructure:

- `control.example.com` -> HTTPS/WSS signaling + credential APIs;
- `turn.example.com` -> coturn public listener.

Deploy TURN geographically close to users. A technically correct single remote relay can still make interactive control feel poor because every media/control packet detours through it.

## Ports

Typical baseline:

- TCP 443: HTTPS/WSS signaling/controller/device credential APIs;
- UDP/TCP 3478: STUN/TURN;
- TCP 5349: TURN/TLS;
- configured UDP relay allocation range, for example `49160-49299`.

Some restrictive networks require TURN/TLS on TCP 443 on a dedicated IP. Do not put an HTTP reverse proxy in front of TURN unless it explicitly supports raw TCP/TLS forwarding.

## Reverse proxy requirements

Terminate HTTPS/WSS with a publicly trusted certificate and forward WebSocket upgrade headers correctly. The backend should normally only be reachable from the trusted proxy/network.

Production settings should include:

```bash
DESKLINK_ALLOW_ANY_ORIGIN=0
DESKLINK_ALLOWED_ORIGINS=https://remote.example.com
DESKLINK_TRUST_PROXY_HEADERS=1
```

Only set `DESKLINK_TRUST_PROXY_HEADERS=1` when clients cannot bypass the trusted proxy; otherwise forwarded client-IP headers can be forged.

Do not log:

- `Authorization` header values;
- controller `desklink-auth.<token>` WebSocket subprotocol values;
- complete host WebSocket query strings containing legacy/native `auth=` bearer tokens;
- Access Codes, device credentials, controller keys or TURN passwords.

Browser runtime controller tokens are intentionally carried in the WebSocket subprotocol request rather than the URL. The server negotiates/echoes only the fixed `desklink-v1` protocol. The native Windows host currently keeps the short-lived host token in the query for compatibility, so reverse-proxy URL logging must be configured accordingly.

## Signaling/device identity

Enable short-lived signaling tokens:

```bash
DESKLINK_SIGNAL_AUTH_SECRET=<long-random-signal-signing-secret>
DESKLINK_SIGNAL_TOKEN_TTL=15m
```

For hosts, prefer the independent device registry:

```bash
DESKLINK_DEVICE_CREDENTIALS_FILE=/run/desklink-secrets/devices.json
```

Provision/rotate a host:

```bash
go run ./tools/auth/rotate-device-registry-credential.go \
  /secure/path/devices.json win-office-01
```

The printed `dc2` credential goes only to that Windows host and is stored there with DPAPI. The server registry stores only its SHA-256 hash.

Legacy `DESKLINK_DEVICE_AUTH_SECRET`/`dc1` remains supported for migration. When a device registry is configured it is authoritative and failures are fail-closed.

## Browser controller identity

Configure the controller registry:

```bash
DESKLINK_CONTROLLER_CREDENTIALS_FILE=/run/desklink-secrets/controllers.json
DESKLINK_CONTROLLER_SESSION_TTL=15m
```

Provision one account and its allowed devices:

```bash
go run ./tools/auth/set-controller-registry-key.go \
  /secure/path/controllers.json alice win-office-01 win-laptop-02
```

The browser user enters the resulting `ck1` key at runtime. `/api/v1/controller-session` returns an in-memory short-lived token bound to the browser peer ID and one exact target device. The signaling hub rechecks that scope on every outgoing auth/offer/ICE message.

A production Web build should use:

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

Do not put a production controller key or static signaling bearer token in a `VITE_*` variable. `VITE_SIGNAL_AUTH_TOKEN` is a controlled-test compatibility mechanism only.

## Host Access Code proof

The reusable host Access Code is no longer part of the WebRTC offer. The browser performs the `auth-request` / one-time HMAC challenge/proof flow before creating the initial PeerConnection offer. The signaling server sees challenge/proof material but does not receive the reusable Access Code.

Use a long random Access Code. The current HMAC challenge mechanism prevents replay but is not a PAKE and therefore does not make a weak short PIN safe from offline guessing.

For unattended Windows use, store the Access Code through:

```powershell
.\desklink-service.exe --store-access-code
```

rather than a machine-wide plaintext environment variable.

## Runtime TURN credentials

Configure coturn with `use-auth-secret`/`static-auth-secret` and set the same secret on signaling:

```bash
DESKLINK_TURN_AUTH_SECRET=<same-secret-as-coturn-static-auth-secret>
DESKLINK_TURN_CREDENTIAL_TTL_SECONDS=43200
```

`GET /api/v1/turn-credentials?deviceId=<target-device-id>` requires a valid short-lived token. It accepts either:

- a host token bound to that same host ID; or
- a controller token whose target scope matches the requested device.

Responses are `no-store`. Clients receive only temporary TURN usernames/passwords; the coturn shared secret never leaves the server.

With `VITE_TURN_RUNTIME_REQUIRED=1`, browser credential-fetch failure does not fall back to static public-bundle credentials. With `DESKLINK_TURN_RUNTIME_REQUIRED=1`, the Windows Agent disables TURN for that newly created PeerConnection instead of silently falling back to a long-lived password. Direct/STUN can still work.

## coturn

Start from `infra/coturn/turnserver.tls.example.conf` for public deployment and replace placeholders. Important requirements:

- use a real hostname and publicly trusted certificate;
- configure `external-ip=PUBLIC_IP/PRIVATE_IP` when appropriate;
- open the complete relay UDP range in cloud and host firewalls;
- keep the TURN auth secret outside source control;
- monitor allocations, bandwidth, packet loss and CPU/network saturation;
- deploy multiple regions as usage grows.

Validate TURN/TLS before browser testing:

```bash
./tools/network/check-turn-tls.sh turn.example.com 5349
```

## Compose secret mounts

The development Compose file supports an optional read-only mount:

```text
infra/secrets -> /run/desklink-secrets
```

Example production-style environment values inside that container:

```bash
DESKLINK_DEVICE_CREDENTIALS_FILE=/run/desklink-secrets/devices.json
DESKLINK_CONTROLLER_CREDENTIALS_FILE=/run/desklink-secrets/controllers.json
DESKLINK_REVOKED_DEVICE_IDS_FILE=/run/desklink-secrets/revoked-devices.txt
```

`infra/secrets/` is gitignored. For a real deployment, prefer Docker/Kubernetes/cloud secret mechanisms and administrator-owned files rather than manually maintaining secrets in the repository checkout.

## Revocation

Configure:

```bash
DESKLINK_REVOKED_DEVICE_IDS_FILE=/run/desklink-secrets/revoked-devices.txt
```

Revocation blocks new host/controller credentials and registrations targeting that device, blocks TURN issuance, blocks new signaling toward the target and closes existing signaling sockets on the keepalive recheck. An unreadable configured revocation file fails closed.

## Optional metrics

Set:

```bash
DESKLINK_METRICS_TOKEN=<random-bearer-token>
```

Then `GET /metricsz` with `Authorization: Bearer <token>` returns aggregate connection/rate-limit/auth/forwarding counters. It returns 404 when the token is not configured. Do not place the metrics token in a browser build.

## Windows host network settings

Example non-secret settings:

```powershell
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_STUN_URL "stun:turn.example.com:3478"
setx /M DESKLINK_TURN_HOST "turn.example.com"
setx /M DESKLINK_TURN_PORT "3478"
setx /M DESKLINK_TURN_TLS_PORT "5349"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

Store durable secrets separately with `desklink-service.exe --store-device-credential` and `--store-access-code`.

## Regional relay policy

Route clients to the nearest healthy TURN region based on measured network performance. Useful East Asia coverage may include Japan/Korea, China-adjacent regions where operationally/legal appropriate, Hong Kong/South China-adjacent regions and Southeast Asia. Do not assume geographic distance equals RTT; measure it.

## Acceptance targets

For each representative route record:

- Direct vs Relay;
- relay protocol UDP/TCP/TLS;
- RTT/jitter/loss;
- decoded FPS and actual resolution;
- bitrate/FPS/resolution adaptation behavior;
- input responsiveness while bandwidth/loss is constrained;
- reconnect/ICE-restart recovery time;
- TURN allocation success and relay saturation.

Run the matrix in `docs/NETWORK_TESTING.md`. A relay route is acceptable only if it stays interactive under real target-region RTT and loss; increasing video bitrate does not fix a distant or overloaded relay.
