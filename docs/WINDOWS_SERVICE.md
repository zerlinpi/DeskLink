# Windows service and unattended-session architecture

DeskLink builds two production Windows executables:

- `desklink-agent.exe` — DXGI capture, hardware H.264, WebRTC, short-lived authentication refresh, and remote input inside the interactive user session.
- `desklink-service.exe` — LocalSystem Windows Service responsible for machine/session lifecycle, protected device identity, and keeping the user-session Agent alive.

Keeping these responsibilities separate follows the intended production architecture: the Service owns durable machine identity and lifecycle, while capture and UI input stay in the interactive session where Windows desktop APIs work correctly.

## What this stage supports

After a user has signed in to Windows, the Service:

1. detects the active console session;
2. starts `desklink-agent.exe` in that user session using `WTSQueryUserToken` + `CreateProcessAsUser`;
3. places the Agent in a kill-on-close Job Object when Windows permits it;
4. notices logon/session-switch events and moves the Agent to the new active console session;
5. restarts an unexpectedly exiting Agent with exponential backoff: approximately 2, 4, 8, 16, 32, then 60 seconds;
6. resets the crash backoff after the Agent has stayed up for at least 60 seconds;
7. gracefully asks the Agent to release remote input and exit before user-session switches or service shutdown;
8. force-terminates the Agent only if it has not exited within the five-second graceful-shutdown window;
9. stores the long-lived device bootstrap credential with machine-scope DPAPI;
10. owns the long-lived credential at runtime and gives the user-session Agent only short-lived signaling tokens through a local authentication broker.

This removes the need to manually launch a console Agent after each Windows login, avoids restart storms, and keeps the durable device credential out of the ordinary user-session Agent process when DPAPI provisioning is used.

## Graceful Service -> Agent shutdown

For each Agent launch, the Service creates a unique Windows Event in the global object namespace. The event ACL allows LocalSystem to signal it while the user-session Agent receives synchronization/wait access only.

The event name is passed only as a launch argument. It carries no credential, input command, SDP, or other remote-control data.

When the Service needs to stop the Agent:

1. it signals the Event;
2. the Agent immediately releases every keyboard key and mouse button currently tracked as injected by DeskLink;
3. the Agent triggers its existing normal shutdown path;
4. `WebRtcSession::Stop()` releases input again and closes realtime state;
5. the process tears down the encoder, GPU conversion, DXGI capture and Media Foundation normally;
6. the Service waits up to five seconds;
7. only if the process is still alive does the Job Object/`TerminateProcess` fallback run.

The input-release operation is intentionally idempotent so shutdown races do not leave a remotely pressed modifier or mouse button stuck.

## DPAPI-protected device credential

The preferred unattended-host configuration does not store the long-lived `dc1...` credential in the machine environment.

From an elevated terminal, run:

```powershell
.\desklink-service.exe --store-device-credential
```

The command prompts for the credential with console echo disabled, then stores a machine-scope DPAPI blob at:

```text
%ProgramData%\DeskLink\device-credential.dpapi
```

The directory and file DACL are protected so only LocalSystem and the local Administrators group receive full access. The Service uses `CryptProtectData(..., CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN)` with DeskLink-specific optional entropy.

To remove it:

```powershell
.\desklink-service.exe --clear-device-credential
```

After migrating to DPAPI, remove any old machine-level plaintext credential:

```powershell
[Environment]::SetEnvironmentVariable(
  "DESKLINK_DEVICE_CREDENTIAL",
  $null,
  [EnvironmentVariableTarget]::Machine
)
```

Then restart the Service.

If the DPAPI file exists but cannot be decrypted or validated, the Service fails that Agent launch and enters its normal retry backoff. It does not silently fall back to a potentially stale machine-wide plaintext credential.

## Service-owned local authentication broker

When a DPAPI credential is present, the long-lived `dc1...` credential remains inside the LocalSystem Service. It is no longer inserted into the Agent environment.

For each Agent launch, the Service creates a unique local Named Pipe and passes only the pipe name to the Agent as a launch argument. The pipe name is an endpoint locator, not a secret credential.

The authentication flow is:

1. the Service decrypts the DPAPI-protected device credential;
2. it creates the target user's normal environment block and removes any inherited `DESKLINK_DEVICE_CREDENTIAL` entry;
3. it starts the Agent without the long-lived credential;
4. the Service starts a local authentication broker bound to that Agent process;
5. the Agent asks the local broker for a signaling token;
6. the Service uses the long-lived credential to call the configured HTTPS `/api/v1/signal-token` endpoint;
7. only the returned short-lived Signal Token and expiry are sent through the Named Pipe to the Agent;
8. the Agent uses the short-lived token for WebSocket registration and to request temporary TURN credentials.

The Service caches a short-lived Signal Token only while it still has more than 90 seconds of lifetime remaining. This reduces unnecessary backend calls and lets brief backend/network interruptions avoid breaking an otherwise valid reconnect. Cached short tokens and in-memory long credential copies are wiped when the broker stops.

### Broker security boundary

The local authentication Pipe uses multiple independent checks:

- the Pipe is local-only with `PIPE_REJECT_REMOTE_CLIENTS`;
- the first Pipe instance is created synchronously before broker startup reports success;
- `FILE_FLAG_FIRST_PIPE_INSTANCE` is used for the initial instance to reject a conflicting pre-existing server instance;
- the Pipe DACL grants access only to LocalSystem and the exact Windows user SID that owns the launched Agent;
- after a connection is accepted, `GetNamedPipeClientProcessId` must exactly match the Agent PID recorded by the Service;
- only the `signal-token` broker command is supported;
- the long-lived device credential is never returned over the Pipe.

The SID check prevents other Windows users on the same machine from opening the Pipe. The separate PID check prevents another process running as the same user from impersonating the Agent.

The Windows CI contains a runtime broker smoke test that verifies:

- the Pipe is already available when broker startup returns;
- an authorized client process can communicate with it;
- unsupported commands are rejected with a structured error;
- a second process running as the same Windows user but with the wrong PID is rejected;
- the broker stops cleanly.

Manual/direct Agent launches without the Service broker continue to support the older environment-variable device-credential path for development compatibility. Production unattended hosts should prefer DPAPI + Service broker mode.

## Important current boundary

This stage does **not** claim Windows Secure Desktop support.

A normal user-session Agent can still be blocked by Windows integrity/UIPI boundaries when:

- the UAC consent/credential desktop is active;
- no user has logged in yet and the Winlogon desktop is active;
- the target application runs at a higher integrity level than the Agent.

Supporting those cases safely requires a separately designed privileged broker/UIAccess path and code-signing/install constraints. DeskLink should not simply run the capture/input Agent as LocalSystem in the user desktop.

## Build output

The normal Windows CMake build produces:

```text
build/windows-agent/Release/desklink-agent.exe
build/windows-agent/Release/desklink-service.exe
```

CI also builds an internal `desklink-service-auth-smoke.exe` runtime test executable, but only the Agent and Service are uploaded as normal release artifacts.

Keep the Agent and Service in the same protected installation directory because the Service resolves `desklink-agent.exe` beside its own executable. A production installer should place them under a directory writable only by administrators, such as `Program Files`.

## Install

Run an elevated terminal from the installation directory:

```powershell
.\desklink-service.exe --install
```

Installation uses:

- service name: `DeskLink`;
- automatic startup;
- LocalSystem service account;
- restart-on-service-failure policy.

The Service attempts to start immediately after installation.

## Uninstall

Run as administrator:

```powershell
.\desklink-service.exe --uninstall
```

## Agent configuration

Keep non-secret machine-wide settings as system environment variables:

```powershell
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_ACCESS_CODE "REPLACE_WITH_SECRET"
setx /M DESKLINK_STUN_URL "stun:turn.example.com:3478"
setx /M DESKLINK_TURN_HOST "turn.example.com"
setx /M DESKLINK_TURN_PORT "3478"
setx /M DESKLINK_TURN_TLS_PORT "5349"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

Store the durable device bootstrap credential separately through the elevated Service command instead of `setx /M DESKLINK_DEVICE_CREDENTIAL`.

`DESKLINK_ACCESS_CODE` is still a secret and remains an environment-variable deployment bridge in the current architecture. It should eventually move to protected Service-owned configuration or be replaced by the account/session authorization model.

`DESKLINK_OUTPUT_INDEX` is read by the Service and passed to the Agent as its monitor index argument.

After changing machine environment variables or protected credentials, restart the `DeskLink` Service so newly launched Agent state receives the new configuration.

See `docs/DEVICE_AUTH.md` for device provisioning, short-lived token renewal, TURN credentials and per-device revocation.

## Security notes

- Protect the installation directory from non-admin writes.
- Do not embed coturn `static-auth-secret` or any server master secret in either Windows executable.
- Do not log registration tokens, TURN passwords, device credentials or remote-control access codes.
- The Service launches the capture/input Agent as the signed-in user rather than LocalSystem. This is intentional.
- DPAPI machine scope protects the durable credential at rest together with the file ACL; local Administrators and SYSTEM remain inside the machine trust boundary.
- The Service broker narrows runtime exposure: the ordinary Agent receives short-lived Signal/TURN material, not the long-lived `dc1` credential.

## Next Windows service milestones

The next Windows security milestones are:

1. replace the deterministic HMAC `dc1` bootstrap model with a real per-device registry and independently rotatable keys;
2. move the remaining unattended access-code secret out of plaintext machine environment configuration;
3. add installer/code-signing hardening;
4. design a narrowly scoped privileged broker/UIAccess path for operations that genuinely require elevation.

Secure Desktop/UAC and Windows logon-screen support should be added only after that privileged trust boundary is explicit, signed and auditable.
