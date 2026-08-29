# Windows service and unattended-session architecture

DeskLink builds two Windows executables:

- `desklink-agent.exe` — DXGI capture, hardware H.264, WebRTC, authentication refresh, and remote input inside the interactive user session.
- `desklink-service.exe` — LocalSystem Windows Service responsible only for machine/session lifecycle and keeping the user-session Agent alive.

Keeping these responsibilities separate follows the intended production architecture: the service owns machine/session lifecycle, while capture and UI input stay in the interactive session where Windows desktop APIs work correctly.

## What this stage supports

After a user has signed in to Windows, the service:

1. detects the active console session;
2. starts `desklink-agent.exe` in that user session using `WTSQueryUserToken` + `CreateProcessAsUser`;
3. places the Agent in a kill-on-close Job Object when Windows permits it;
4. notices logon/session-switch events and moves the Agent to the new active console session;
5. restarts an unexpectedly exiting Agent with exponential backoff: approximately 2, 4, 8, 16, 32, then 60 seconds;
6. resets the crash backoff after the Agent has stayed up for at least 60 seconds;
7. gracefully asks the Agent to release remote input and exit before user-session switches or service shutdown;
8. force-terminates the Agent only if it has not exited within the five-second graceful-shutdown window.

This removes the need to manually launch a console Agent after each Windows login and avoids a rapid restart storm when capture/driver initialization is persistently failing.

## Graceful Service -> Agent shutdown

For each Agent launch, the Service creates a unique Windows Event in the global object namespace. The event ACL allows LocalSystem to signal it while authenticated user processes receive only synchronization/wait access.

The event name is passed only as a launch argument. It carries no credential, input command, SDP, or other remote-control data.

When the Service needs to stop the Agent:

1. it signals the Event;
2. the Agent immediately releases every keyboard key and mouse button currently tracked as injected by DeskLink;
3. the Agent triggers its existing normal shutdown path;
4. `WebRtcSession::Stop()` releases input again, closes realtime state, and the process then tears down the encoder, GPU conversion, DXGI capture and Media Foundation normally;
5. the Service waits up to five seconds;
6. only if the process is still alive does the Job Object/`TerminateProcess` fallback run.

The input-release operation is intentionally idempotent so shutdown races do not leave a remotely pressed modifier or mouse button stuck.

## Important current boundary

This stage does **not** claim Windows Secure Desktop support.

A normal user-session Agent can still be blocked by Windows integrity/UIPI boundaries when:

- the UAC consent/credential desktop is active;
- no user has logged in yet and the Winlogon desktop is active;
- the target application runs at a higher integrity level than the Agent.

Supporting those cases safely requires a separately designed privileged broker/UIAccess path and code-signing/install constraints. DeskLink should not simply run the capture/input Agent as LocalSystem in the user desktop.

## Build output

The normal Windows CMake build produces both executables in the Release output directory:

```text
build/windows-agent/Release/desklink-agent.exe
build/windows-agent/Release/desklink-service.exe
```

Keep both files in the same protected installation directory because the service resolves `desklink-agent.exe` beside its own executable.

A production installer should place them under a directory writable only by administrators, such as `Program Files`.

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

The service attempts to start immediately after installation.

## Uninstall

Run as administrator:

```powershell
.\desklink-service.exe --uninstall
```

## Agent configuration

The service launches the Agent with the active user's environment plus machine environment variables. For unattended operation, configure DeskLink settings as **machine/system environment variables** rather than relying only on a terminal's temporary environment.

For the current unattended authentication flow:

```powershell
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_DEVICE_CREDENTIAL "dc1.REPLACE_ME"
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

`DESKLINK_OUTPUT_INDEX` is read by the service and passed to the Agent as its monitor index argument.

After changing machine environment variables, restart the `DeskLink` service so a newly launched Agent receives the new environment.

See `docs/DEVICE_AUTH.md` for provisioning, short-lived signal-token renewal, TURN credentials, and per-device revocation.

## Security notes

- Protect the installation directory from non-admin writes.
- Do not embed coturn `static-auth-secret` or any server master secret in either Windows executable.
- Prefer temporary TURN credentials issued by the authenticated signaling/backend flow.
- Do not log registration tokens, TURN passwords, device credentials, or remote-control access codes.
- The service launches the Agent as the signed-in user rather than LocalSystem. This is intentional.
- Machine environment variables are still a transitional secret-storage mechanism. A hardened installer should move the long-lived device credential to a Service-owned Windows-protected store and pass only short-lived credentials into the user session.

## Next Windows service milestone

The next security milestone is protected device-secret storage plus a narrowly scoped privileged broker for operations that genuinely require elevation. Secure Desktop/UAC support should be added only after that trust boundary is explicit, signed, and auditable.
