# Unattended device authentication

DeskLink separates a long-lived machine bootstrap credential from short-lived signaling and TURN credentials.

## Credential chain

For an unattended Windows host, the intended production flow is:

1. The server keeps `DESKLINK_DEVICE_AUTH_SECRET`, `DESKLINK_SIGNAL_AUTH_SECRET` and `DESKLINK_TURN_AUTH_SECRET` private.
2. An administrator provisions one long-lived device credential for an exact `deviceId`.
3. The Windows host stores only that device credential plus the public HTTPS endpoint URLs.
4. Before connecting to signaling, the Agent sends the device credential only to `GET /api/v1/signal-token?deviceId=...` over HTTPS.
5. The server returns a short-lived signaling registration token. Default lifetime is 15 minutes and the server refuses configured lifetimes above one hour.
6. The Agent uses that short-lived token in the WebSocket registration request.
7. Before creating a new PeerConnection, the Agent refreshes the short-lived signaling token again and uses it to request temporary TURN REST credentials.
8. The coturn shared secret never leaves the server/relay configuration.

The long-lived device credential is never placed in a WebSocket URL, SDP, ICE candidate, DataChannel message or TURN password.

## Server configuration

Use independent long random values for the three server secrets:

```bash
DESKLINK_DEVICE_AUTH_SECRET=<device-bootstrap-master-secret>
DESKLINK_SIGNAL_AUTH_SECRET=<short-lived-signal-token-secret>
DESKLINK_SIGNAL_TOKEN_TTL=15m
DESKLINK_TURN_AUTH_SECRET=<same-value-as-coturn-static-auth-secret>
DESKLINK_TURN_CREDENTIAL_TTL_SECONDS=43200
```

`/api/v1/signal-token` is not available unless both the device-auth and signal-auth secrets are configured.

## Provision a Windows host

Generate a credential offline on a trusted administrator machine. Do not run this command on the public signaling server shell if untrusted users can observe process environments or terminal history.

```powershell
$env:DESKLINK_DEVICE_AUTH_SECRET = "<device-bootstrap-master-secret>"
go run .\tools\auth\mint-device-credential.go win-office-01
```

Copy only the printed `dc1....` value to that Windows host. Do not copy `DESKLINK_DEVICE_AUTH_SECRET` to the host.

Configure the host as machine-level settings for the current Service-based deployment:

```powershell
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_DEVICE_CREDENTIAL "dc1.REPLACE_ME"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

Restart the DeskLink Service after changing machine environment variables.

## Failure behavior

With `DESKLINK_SIGNAL_TOKEN_REQUIRED=1`, the Agent does not attempt signaling registration with a stale/static token if device-token exchange fails. It enters the existing exponential signaling reconnect loop and retries later.

With `DESKLINK_TURN_RUNTIME_REQUIRED=1`, a failed TURN credential exchange disables TURN for that new PeerConnection instead of falling back to a long-lived static TURN password. Direct/STUN ICE can still work when the network permits it.

## Current security boundary and limitation

The `dc1` credential is currently derived from a server master secret and the exact `deviceId` using HMAC-SHA256. This is a bootstrap architecture that avoids a database, but it has an important limitation: there is no per-device revocation list. Revoking one derived credential currently requires rotating the device-auth master secret, which invalidates every derived device credential.

Before a public multi-user release, replace or extend this with a device registry that stores independent per-device credential hashes/keys and supports individual revocation, rotation, ownership transfer and audit history.

Machine environment variables are also a deployment bridge, not the final Windows secret store. A hardened installer should move the long-lived device credential into Windows-protected storage (for example a service-owned DPAPI-protected secret) and pass only short-lived material to the user-session Agent.

## Browser controllers

Do not provision the long-lived Windows device credential into browser JavaScript. Browser users need a separate account authentication flow that issues a short-lived controller registration token at runtime. `VITE_SIGNAL_AUTH_TOKEN` remains a controlled-test mechanism because Vite variables are embedded in the static bundle.
