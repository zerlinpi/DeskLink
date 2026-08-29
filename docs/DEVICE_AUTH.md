# Unattended device authentication

DeskLink separates a long-lived machine bootstrap credential from short-lived signaling and TURN credentials.

## Credential chain

For an unattended Windows host, the intended production flow is:

1. The server keeps `DESKLINK_DEVICE_AUTH_SECRET`, `DESKLINK_SIGNAL_AUTH_SECRET` and `DESKLINK_TURN_AUTH_SECRET` private.
2. An administrator provisions one long-lived device credential for an exact `deviceId`.
3. The Windows Service stores that credential with machine-scope Windows DPAPI under `%ProgramData%\DeskLink`.
4. The Service decrypts it only when launching the user-session Agent and injects it into that child process environment.
5. Before connecting to signaling, the Agent sends the device credential only to `GET /api/v1/signal-token?deviceId=...` over HTTPS.
6. The server returns a short-lived signaling registration token. Default lifetime is 15 minutes and the server refuses configured lifetimes above one hour.
7. The Agent uses that short-lived token in the WebSocket registration request.
8. Before creating a new PeerConnection, the Agent refreshes the short-lived signaling token again and uses it to request temporary TURN REST credentials.
9. The coturn shared secret never leaves the server/relay configuration.

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

Configure non-secret host settings as machine-level values:

```powershell
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

Then, from an elevated terminal in the DeskLink installation directory, store the long-lived credential using the Service-owned DPAPI store:

```powershell
.\desklink-service.exe --store-device-credential
```

Paste the `dc1....` value when prompted. Console echo is disabled while the credential is entered.

The encrypted blob is stored at:

```text
%ProgramData%\DeskLink\device-credential.dpapi
```

The file and directory ACL are restricted to LocalSystem and local Administrators. The Service uses machine-scope DPAPI and never prints the credential.

Restart the DeskLink Service after provisioning:

```powershell
Restart-Service DeskLink
```

To remove the protected credential:

```powershell
.\desklink-service.exe --clear-device-credential
```

### Migration from the old environment-variable method

For backward compatibility, if no protected DPAPI credential exists, the Service still launches the Agent with its normal environment. This keeps existing development/test installations working.

When a DPAPI credential exists, it takes precedence: the Service removes any inherited `DESKLINK_DEVICE_CREDENTIAL` entry from the child environment and inserts the decrypted protected value instead.

After verifying the DPAPI migration, remove the old plaintext machine variable:

```powershell
[Environment]::SetEnvironmentVariable(
  "DESKLINK_DEVICE_CREDENTIAL",
  $null,
  [EnvironmentVariableTarget]::Machine
)
Restart-Service DeskLink
```

If the DPAPI file exists but is corrupt, unreadable or cannot be decrypted, Agent launch fails closed and enters the Service retry backoff. DeskLink does not silently fall back to a stale plaintext credential in that case.

## Revoke one device

DeskLink can revoke one `deviceId` without rotating the master device secret.

For a small emergency list, set a comma/space/newline-separated value:

```bash
DESKLINK_REVOKED_DEVICE_IDS=win-office-01,win-laptop-02
```

For production, prefer a root/admin-owned read-only text file and point the signaling service at it:

```bash
DESKLINK_REVOKED_DEVICE_IDS_FILE=/run/secrets/desklink-revoked-devices
```

The file may contain one device ID per line or comma/space-separated values. It is read on authentication/credential checks rather than cached, so editing the file takes effect without restarting the signaling process.

A revoked device is denied at all current credential boundaries:

- `/api/v1/signal-token` returns `403` even when the device presents the correct long-lived credential;
- `/api/v1/turn-credentials` returns `403` even when the device still has an unexpired short-lived signal token;
- new WebSocket registration is rejected;
- existing signaling WebSockets are rechecked on the 30-second keepalive cycle and are actively closed after revocation;
- another peer is not allowed to start new signaling toward a revoked target.

If `DESKLINK_REVOKED_DEVICE_IDS_FILE` is configured but cannot be read, authentication/credential operations fail closed instead of silently ignoring the revocation source.

## Failure behavior

With `DESKLINK_SIGNAL_TOKEN_REQUIRED=1`, the Agent does not attempt signaling registration with a stale/static token if device-token exchange fails. It enters the existing exponential signaling reconnect loop and retries later.

With `DESKLINK_TURN_RUNTIME_REQUIRED=1`, a failed TURN credential exchange disables TURN for that new PeerConnection instead of falling back to a long-lived static TURN password. Direct/STUN ICE can still work when the network permits it.

## Current security boundary and limitation

The `dc1` credential is currently derived from a server master secret and the exact `deviceId` using HMAC-SHA256. The revocation list allows an individual device to be disabled without invalidating every other host.

However, `dc1` remains deterministic for a given master secret and `deviceId`. If that credential is leaked, removing the device from the revocation list would make the leaked credential valid again. A truly independent per-device credential rotation still requires a device registry that stores separate per-device credential hashes/keys and supports rotation, ownership transfer and audit history.

DPAPI now protects the long-lived device credential **at rest** on Windows. This is not yet the final runtime boundary: the user-session Agent still receives the credential in its process environment because the Agent currently performs the HTTPS short-token exchange itself. The next hardening step is a Service-owned local authentication broker so the long-lived credential never enters the user-session Agent and the Agent receives only short-lived signal/TURN material.

Local administrators and LocalSystem remain inside the machine trust boundary for machine-scope DPAPI.

## Browser controllers

Do not provision the long-lived Windows device credential into browser JavaScript. Browser users need a separate account authentication flow that issues a short-lived controller registration token at runtime. `VITE_SIGNAL_AUTH_TOKEN` remains a controlled-test mechanism because Vite variables are embedded in the static bundle.
