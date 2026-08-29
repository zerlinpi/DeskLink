# Unattended device authentication

DeskLink separates durable machine identity from short-lived signaling/TURN credentials. Production hosts should use independently rotatable `dc2` credentials plus the Windows Service/DPAPI broker path.

## Production credential chain

For an unattended Windows host:

1. The signaling service keeps `DESKLINK_SIGNAL_AUTH_SECRET` and `DESKLINK_TURN_AUTH_SECRET` private.
2. An administrator creates one random `dc2` credential for an exact `deviceId`; the server registry stores only its SHA-256 hash.
3. The Windows Service stores the `dc2` credential with machine-scope DPAPI under `%ProgramData%\DeskLink`.
4. At runtime the LocalSystem Service decrypts the credential, but does **not** place it in the user-session Agent environment.
5. A PID/SID-bound local Named Pipe broker uses the credential to call `/api/v1/signal-token` over HTTPS.
6. The user-session Agent receives only the short-lived Signal Token and expiry from the broker.
7. The Agent uses that short token for signaling registration and to request temporary TURN REST credentials.
8. The coturn shared secret never leaves the server/relay configuration.

The durable device credential is not sent in SDP, ICE, DataChannels, TURN passwords, or browser JavaScript.

## Server configuration

Preferred production settings:

```bash
DESKLINK_DEVICE_CREDENTIALS_FILE=/run/desklink-secrets/devices.json
DESKLINK_SIGNAL_AUTH_SECRET=<long-random-signal-signing-secret>
DESKLINK_SIGNAL_TOKEN_TTL=15m
DESKLINK_TURN_AUTH_SECRET=<same-long-random-secret-as-coturn-static-auth-secret>
DESKLINK_TURN_CREDENTIAL_TTL_SECONDS=43200
DESKLINK_REVOKED_DEVICE_IDS_FILE=/run/desklink-secrets/revoked-devices.txt
```

The supplied Compose file can mount `infra/secrets` read-only at `/run/desklink-secrets`. Keep that directory outside source control; `.gitignore` excludes it. Production deployments should normally inject the files through a proper secret/config mechanism rather than treating the repository directory itself as a secret store.

### Legacy dc1 compatibility

`DESKLINK_DEVICE_AUTH_SECRET` and `tools/auth/mint-device-credential.go` remain supported for legacy `dc1` migration/testing. A `dc1` value is deterministically derived from one server master secret plus `deviceId`.

When `DESKLINK_DEVICE_CREDENTIALS_FILE` is configured, the registry becomes authoritative: unreadable/invalid registry state fails closed and does not fall back to `DESKLINK_DEVICE_AUTH_SECRET`.

## Create or rotate a dc2 host credential

On a trusted administrator machine:

```bash
go run ./tools/auth/rotate-device-registry-credential.go \
  /secure/path/devices.json win-office-01
```

The tool prints a new `dc2....` credential once. The JSON file stores only the credential SHA-256 hash. Run the same command again to rotate only that device; the signaling server reads the registry at authentication time, so the old credential stops working without a signaling-process restart.

Device/account IDs use the same server rule everywhere: ASCII letters/digits plus `-`, `_`, `.` and at most 128 bytes.

## Configure the Windows host

Store non-secret machine settings normally:

```powershell
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_STUN_URL "stun:turn.example.com:3478"
setx /M DESKLINK_TURN_HOST "turn.example.com"
setx /M DESKLINK_TURN_PORT "3478"
setx /M DESKLINK_TURN_TLS_PORT "5349"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

From an elevated terminal beside `desklink-service.exe`, store the durable credential:

```powershell
.\desklink-service.exe --store-device-credential
```

Paste the `dc2....` value when prompted. Console echo is disabled. Both `dc2` and legacy `dc1` values are accepted.

The encrypted blob is stored at:

```text
%ProgramData%\DeskLink\device-credential.dpapi
```

The directory/file ACL is restricted to LocalSystem and local Administrators. Machine-scope DPAPI plus the ACL protects the value at rest; LocalSystem and local Administrators remain inside the machine trust boundary.

Restart the Service after provisioning:

```powershell
Restart-Service DeskLink
```

To remove the protected credential:

```powershell
.\desklink-service.exe --clear-device-credential
```

## Protect the unattended Access Code

The remote-control Access Code is a separate secret. Store it through the Service instead of a machine-wide `DESKLINK_ACCESS_CODE` variable:

```powershell
.\desklink-service.exe --store-access-code
Restart-Service DeskLink
```

It is stored as:

```text
%ProgramData%\DeskLink\access-code.dpapi
```

When protected Access Code storage is enabled, the Service removes any inherited `DESKLINK_ACCESS_CODE` from the Agent environment. The Agent requests the Access Code through the same PID/SID-bound broker capability during startup. A declared broker capability that cannot supply the value fails closed.

Remove it with:

```powershell
.\desklink-service.exe --clear-access-code
```

After migration, remove old plaintext machine variables:

```powershell
[Environment]::SetEnvironmentVariable(
  "DESKLINK_DEVICE_CREDENTIAL",
  $null,
  [EnvironmentVariableTarget]::Machine
)
[Environment]::SetEnvironmentVariable(
  "DESKLINK_ACCESS_CODE",
  $null,
  [EnvironmentVariableTarget]::Machine
)
Restart-Service DeskLink
```

If a DPAPI file exists but is corrupt/unreadable, DeskLink does not silently fall back to the corresponding plaintext machine value.

## Access Code over the network

The browser does not send the reusable Access Code inside the WebRTC offer. It proves knowledge through the one-time `hmac-sha256-v1` challenge flow documented in `docs/SIGNAL_AUTH.md`.

The challenge is 256-bit random, lasts 15 seconds, is bound to controller/host/session, and is consumed on the first proof attempt. The signaling service sees only challenge/proof material, not a reusable Access Code.

Because HMAC challenge-response is not a PAKE, weak short PINs remain susceptible to offline guessing by an observer with challenge/proof material. Use a long random Access Code in production until a PAKE/OPAQUE-style protocol is introduced.

## Revoke one device

For a small emergency list:

```bash
DESKLINK_REVOKED_DEVICE_IDS=win-office-01,win-laptop-02
```

For production, prefer an administrator-owned file:

```bash
DESKLINK_REVOKED_DEVICE_IDS_FILE=/run/desklink-secrets/revoked-devices.txt
```

The file is reread on authorization checks. A revoked device is denied at current credential boundaries:

- `/api/v1/signal-token` refuses even a correct durable credential;
- `/api/v1/turn-credentials` refuses host/controller tokens scoped to that target;
- new host or target-scoped controller WebSocket registration is rejected;
- existing signaling sockets are rechecked on the keepalive cycle and closed after revocation;
- new signaling toward the revoked target is denied.

If the configured revocation file cannot be read, authorization fails closed.

## Failure behavior

With `DESKLINK_SIGNAL_TOKEN_REQUIRED=1`, failure to exchange the durable credential for a fresh short token defers signaling and enters the Agent reconnect loop; no stale fallback token is silently used.

With `DESKLINK_TURN_RUNTIME_REQUIRED=1`, failed TURN credential exchange disables TURN for the new PeerConnection rather than using a long-lived static TURN password. Direct/STUN ICE can still succeed.

## Browser controllers

Browser controllers never receive Windows device credentials. They use the separate controller registry/runtime `ct1` session flow described in `docs/SIGNAL_AUTH.md`.

Do not put production controller keys or signaling bearer tokens in `VITE_*` variables. `VITE_SIGNAL_AUTH_TOKEN` is retained only as a controlled development/migration mechanism because Vite values are embedded into the static bundle.
