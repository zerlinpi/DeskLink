# Signaling and controller authentication

DeskLink separates three credentials with different responsibilities:

- a long-lived **device bootstrap credential** (`dc2` preferred, legacy `dc1` supported) used only to obtain short-lived host tokens;
- a long-lived **controller key** (`ck1`) whose SHA-256 hash and allowed devices live in the server-side controller registry;
- short-lived **signaling/controller session tokens** signed by `DESKLINK_SIGNAL_AUTH_SECRET`.

The Windows remote-control Access Code is a separate host secret and is not a signaling registration credential.

## Host registration

When `DESKLINK_SIGNAL_AUTH_SECRET` is configured, a Windows host must present a valid short-lived token bound to its exact `deviceId`. The host obtains that token from:

```text
GET /api/v1/signal-token?deviceId=<device-id>
Authorization: Bearer <dc1-or-dc2-device-credential>
```

The response is `no-store`. The default lifetime is 15 minutes and configured issuance is capped at one hour.

For compatibility with the native Agent, host WebSocket registration currently uses:

```text
wss://control.example.com/ws?deviceId=win-office-01&auth=<short-lived-host-token>
```

Do not log full signaling query strings at the reverse proxy. The token is short-lived, but it is still a bearer credential until expiry.

When `DESKLINK_SIGNAL_AUTH_SECRET` is empty, unauthenticated WebSocket registration remains available for local development only.

## Independent device credentials

Production should configure:

```bash
DESKLINK_DEVICE_CREDENTIALS_FILE=/run/desklink-secrets/devices.json
```

Use the provisioning tool to create or rotate one random `dc2` credential for one exact device ID:

```bash
go run ./tools/auth/rotate-device-registry-credential.go \
  /secure/path/devices.json win-office-01
```

The command prints the new `dc2...` value once. The registry stores only its SHA-256 hash. Running it again for the same device immediately rotates only that device without changing other hosts or restarting the signaling service.

If `DESKLINK_DEVICE_CREDENTIALS_FILE` is configured, registry errors fail closed and DeskLink does **not** fall back to the legacy master-secret derivation. `DESKLINK_DEVICE_AUTH_SECRET` + `dc1` remains a migration/development path.

## Browser controller sessions

Do not put a long-lived controller secret or production signaling token in `VITE_*` build variables. Vite values are embedded in public JavaScript.

Production browser authentication uses a server-side controller registry:

```bash
DESKLINK_CONTROLLER_CREDENTIALS_FILE=/run/desklink-secrets/controllers.json
DESKLINK_CONTROLLER_SESSION_TTL=15m
```

Provision/rotate an account and the exact devices it may control:

```bash
go run ./tools/auth/set-controller-registry-key.go \
  /secure/path/controllers.json alice win-office-01 win-laptop-02
```

The command prints a new random `ck1...` key. The registry stores only the SHA-256 hash and the allow-list. The browser user enters the account ID and key at runtime.

The browser then sends:

```text
POST /api/v1/controller-session
Content-Type: application/json

{
  "accountId": "alice",
  "controllerId": "web-1234abcd",
  "targetDeviceId": "win-office-01",
  "accessKey": "ck1...."
}
```

The response contains a short-lived `ct1` token bound to all of:

- the browser `controllerId`;
- the one authorized target `deviceId`;
- an expiry timestamp.

The browser keeps this token only in memory. It is refreshed when necessary and is cleared on disconnect.

### WebSocket token transport

Runtime controller tokens are intentionally kept out of the WebSocket URL. The browser requests these WebSocket subprotocols:

```text
desklink-v1
desklink-auth.<ct1-token>
```

The signaling server parses the auth-bearing request value but negotiates/echoes only the fixed `desklink-v1` subprotocol. This reduces accidental bearer-token retention in ordinary URL/access logs.

The legacy `?auth=` query remains supported for the native host and controlled compatibility tests. If both are present, the legacy query takes precedence.

## Target authorization

A valid controller session is not a general signaling pass. The signaling hub stores the target scope from the `ct1` token and checks every outgoing offer/ICE/auth message. A controller cannot redirect an already-issued token to another device.

The browser also checks that host-scoped `auth-*`, SDP answer and ICE messages come from the expected target and current session before processing them.

Revoking the target device invalidates new controller sessions, TURN issuance and WebSocket registration for controllers scoped to that target; existing signaling connections are rechecked on the keepalive cycle.

## TURN credentials

`GET /api/v1/turn-credentials?deviceId=...` accepts either:

- a host signaling token for the same host ID; or
- a controller `ct1` token whose authorized target matches the requested target device.

The coturn `static-auth-secret` remains server-side. Clients receive only temporary TURN REST credentials.

## Host Access Code proof

The controller no longer places the reusable Access Code inside the WebRTC offer. Before the first offer it performs:

```text
auth-request
  -> auth-challenge { nonce, algorithm: hmac-sha256-v1 }
auth-proof { proof }
  -> auth-accepted
  -> offer { sdp, type }
```

The Windows host creates a 256-bit CSPRNG nonce valid for 15 seconds and only one proof attempt. The proof is HMAC-SHA256 over a canonical string containing the controller ID, host ID, signaling session ID and nonce. A successful proof authorizes one initial offer for 15 seconds. An already-authorized live PeerConnection may subsequently renegotiate for ICE restart without resending the reusable Access Code.

This prevents the signaling service from receiving a reusable Access Code and prevents replay of an observed proof against a different nonce/session/device. It is **not a PAKE**: a weak human PIN can still be vulnerable to offline guessing by an observer that has the challenge and proof. Production deployments should therefore use a long random Access Code until DeskLink adopts a PAKE/OPAQUE-style low-entropy-password protocol.

## Deployment rules

For public deployment:

- use HTTPS/WSS with publicly trusted certificates;
- set `DESKLINK_ALLOWED_ORIGINS` and disable `DESKLINK_ALLOW_ANY_ORIGIN`;
- use independent random values for signal and TURN server secrets;
- keep `devices.json`, `controllers.json` and revocation files administrator-owned, non-world-writable and outside source control;
- configure the reverse proxy not to log sensitive Authorization headers or full host `auth` query strings;
- keep controller keys, device credentials, Access Codes and TURN shared secrets out of application logs;
- use high-entropy Access Codes; do not rely on a short numeric PIN with the current HMAC proof protocol.
