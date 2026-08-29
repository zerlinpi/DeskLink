# Windows service and unattended-session architecture

DeskLink now builds two Windows executables:

- `desklink-agent.exe` — DXGI capture, hardware H.264, WebRTC, and remote input inside the interactive user session.
- `desklink-service.exe` — LocalSystem Windows Service responsible only for keeping the user-session Agent alive.

Keeping these responsibilities separate follows the intended production architecture: the service owns machine/session lifecycle, while capture and UI input stay in the interactive session where Windows desktop APIs work correctly.

## What this stage supports

After a user has signed in to Windows, the service:

1. detects the active console session;
2. starts `desklink-agent.exe` in that user session using `WTSQueryUserToken` + `CreateProcessAsUser`;
3. places the Agent in a kill-on-close Job Object when Windows permits it;
4. notices logon/session-switch events and moves the Agent to the new active console session;
5. restarts the Agent after an unexpected exit with a retry delay;
6. stops the Agent when the service stops.

This removes the need to manually launch a console Agent after each Windows login.

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

Examples:

```powershell
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_AUTH_TOKEN "SHORT_LIVED_DEVICE_TOKEN"
setx /M DESKLINK_ACCESS_CODE "REPLACE_WITH_SECRET"
setx /M DESKLINK_STUN_URL "stun:turn.example.com:3478"
setx /M DESKLINK_TURN_HOST "turn.example.com"
setx /M DESKLINK_TURN_PORT "3478"
setx /M DESKLINK_TURN_TLS_PORT "5349"
```

`DESKLINK_OUTPUT_INDEX` is read by the service and passed to the Agent as its monitor index argument.

After changing machine environment variables, restart the `DeskLink` service so a newly launched Agent receives the new environment.

## Security notes

- Protect the installation directory from non-admin writes.
- Do not embed coturn `static-auth-secret` in either Windows executable.
- Prefer temporary TURN credentials issued by the authenticated signaling/backend flow.
- Do not log registration tokens, TURN passwords, or remote-control access codes.
- The service launches the Agent as the signed-in user rather than LocalSystem. This is intentional.

## Next Windows service milestone

The next milestone is a privileged broker protocol for narrowly scoped operations that genuinely require elevation, plus installer/signing hardening. Secure Desktop/UAC support should be added only after that trust boundary is explicit and auditable.
