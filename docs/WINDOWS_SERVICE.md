# Windows Service and unattended host

`desklink-service.exe` is the LocalSystem supervisor for logged-in unattended DeskLink hosts. It does not capture the desktop itself. The Service launches `desklink-agent.exe` inside the active interactive user session so DXGI capture, hardware encoding and `SendInput` run in the correct session.

## What the Service currently provides

- automatic start with Windows;
- launch/relaunch of the Agent in the active console user session;
- reaction to logon/logoff/session changes;
- exponential backoff for Agent crash loops;
- a Job Object with kill-on-close containment;
- graceful Service -> Agent stop event with a five-second cleanup window before forced termination;
- machine-scope DPAPI storage for the durable device credential and unattended Access Code;
- a LocalSystem-owned Named Pipe authentication broker bound to the exact Agent PID and user SID;
- short-lived Signal Token exchange/cache without exposing the durable device credential to the user-session Agent.

This is **logged-in unattended persistence**. Windows logon screen/UAC Secure Desktop capture/control is still outside the current trust boundary and needs a separate narrowly scoped privileged broker/session design.

## Build

```powershell
cmake -S apps/windows-agent -B build/windows-agent -A x64
cmake --build build/windows-agent --config Release --parallel
```

The Release directory contains:

```text
desklink-agent.exe
desklink-service.exe
```

GitHub Actions also uploads both binaries after a successful Windows job.

## Configure non-secret machine settings

Example production-style machine variables:

```powershell
setx /M DESKLINK_DEVICE_ID "win-office-01"
setx /M DESKLINK_SIGNAL_URL "wss://control.example.com/ws"
setx /M DESKLINK_SIGNAL_TOKEN_URL "https://control.example.com/api/v1/signal-token"
setx /M DESKLINK_SIGNAL_TOKEN_REQUIRED "1"
setx /M DESKLINK_STUN_URL "stun:turn.example.com:3478"
setx /M DESKLINK_TURN_HOST "turn.example.com"
setx /M DESKLINK_TURN_PORT "3478"
setx /M DESKLINK_TURN_TLS_PORT "5349"
setx /M DESKLINK_TURN_CREDENTIALS_URL "https://control.example.com/api/v1/turn-credentials"
setx /M DESKLINK_TURN_RUNTIME_REQUIRED "1"
```

Video/performance variables such as `DESKLINK_FPS`, `DESKLINK_BITRATE_BPS`, `DESKLINK_MAX_WIDTH` and `DESKLINK_MAX_HEIGHT` can also be configured machine-wide because they are not credentials.

## Store the durable device credential

Prefer a random `dc2` generated from the server-side per-device registry. Legacy `dc1` is accepted for migration.

From an elevated terminal:

```powershell
.\desklink-service.exe --store-device-credential
```

Paste the credential when prompted. Console echo is disabled.

The Service stores the encrypted value at:

```text
%ProgramData%\DeskLink\device-credential.dpapi
```

The directory and file DACL are protected so only LocalSystem and local Administrators have access.

## Store the unattended Access Code

Use a long random Access Code, then store it through the Service:

```powershell
.\desklink-service.exe --store-access-code
```

It is protected separately at:

```text
%ProgramData%\DeskLink\access-code.dpapi
```

The current validator accepts 8-256 printable characters, but production should use a substantially longer random value because the current network challenge protocol is HMAC-based rather than a PAKE.

## Runtime secret boundary

When protected device credential storage exists:

- the Service removes inherited `DESKLINK_DEVICE_CREDENTIAL` from the child environment;
- the durable credential remains in LocalSystem memory;
- the Agent asks the local broker for a short-lived Signal Token;
- the broker performs the HTTPS `/api/v1/signal-token` exchange and caches the result with a 90-second expiry safety margin;
- the Agent receives only the short token and expiry.

When protected Access Code storage exists:

- the Service removes inherited `DESKLINK_ACCESS_CODE` from the child environment;
- the Service starts the broker with the `access-code` capability;
- the Agent reads the Access Code from the broker during startup;
- if the capability is declared but the broker cannot supply the value, Agent startup fails closed instead of falling back to a stale environment value.

The two broker capabilities are independent. A deployment can migrate device identity and Access Code separately.

## Named Pipe hardening

The Service creates a unique local-only pipe for each Agent launch. The broker:

- creates the first pipe instance synchronously before the suspended Agent is resumed;
- builds an ACL for LocalSystem plus the exact Agent user SID;
- verifies that the connecting Named Pipe client PID equals the exact Agent process ID;
- supports only the declared capabilities (`signal-token`, `access-code`);
- rejects wrong-PID clients even when they run under the same user account.

The protected Agent process is created suspended in broker mode, assigned to its Job Object, the broker is made ready, and only then is the Agent thread resumed.

## Install and restart

From an elevated shell beside the binaries:

```powershell
.\desklink-service.exe --install
Restart-Service DeskLink
```

The service is configured for automatic start and has restart failure actions. The Service watches the active console session and starts an Agent when a usable logged-in session exists.

## Graceful shutdown

On service stop, session replacement or planned Agent restart:

1. the Service signals the unique Agent stop event;
2. the Agent releases remotely-held keys/buttons;
3. signaling, PeerConnection, encoder, GPU conversion and capture are closed normally;
4. the Service waits up to five seconds;
5. only if the Agent does not exit does the Service terminate the Job/process.

This avoids leaving synthetic modifier keys/buttons logically held after a normal Service operation.

## Crash-loop behavior

Unexpected Agent exits use exponential relaunch backoff. A sufficiently stable run resets the failure streak. Session changes reset the prior Agent and launch state for the new active session.

## Remove protected secrets

```powershell
.\desklink-service.exe --clear-access-code
.\desklink-service.exe --clear-device-credential
```

After a successful DPAPI migration, remove any old plaintext variables:

```powershell
[Environment]::SetEnvironmentVariable("DESKLINK_DEVICE_CREDENTIAL", $null, [EnvironmentVariableTarget]::Machine)
[Environment]::SetEnvironmentVariable("DESKLINK_ACCESS_CODE", $null, [EnvironmentVariableTarget]::Machine)
Restart-Service DeskLink
```

If a protected DPAPI file exists but is corrupt/unreadable, Service/Agent launch fails closed for that capability. It does not silently recover by using the old plaintext variable.

## Uninstall

```powershell
.\desklink-service.exe --uninstall
```

Uninstalling the service does not automatically delete `%ProgramData%\DeskLink` secret files. Clear the protected secrets explicitly if the machine is being decommissioned.

## Remaining privileged-session limitation

The current Service deliberately leaves capture and input in the normal interactive user session. This means it does not yet provide reliable control of:

- the Windows sign-in screen;
- UAC Secure Desktop consent UI;
- other desktops isolated from the normal user session.

Adding that capability should not move the entire media/network Agent into LocalSystem. The intended direction is a minimal privileged broker with narrowly scoped capture/input operations and authenticated IPC to the normal Agent.
