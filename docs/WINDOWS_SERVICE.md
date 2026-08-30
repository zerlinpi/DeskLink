# Windows Service and unattended host

`desklink-service.exe` is the LocalSystem supervisor for DeskLink Windows hosts. It deliberately does **not** run the full capture/WebRTC stack as LocalSystem. Instead, it launches `desklink-agent.exe` inside the selected active interactive user session so DXGI capture, hardware encoding and normal-desktop `SendInput` execute in the correct session while durable machine secrets remain behind a Service-owned boundary.

## What the Service currently provides

- automatic start with Windows;
- launch/relaunch of the Agent in the active interactive user session;
- active console preference with fallback to a genuine `WTSActive` RDP session;
- reaction to logon/logoff/session changes and Fast User Switching notifications;
- exponential backoff for Agent crash loops;
- a Job Object with kill-on-close containment;
- graceful Service -> Agent stop event with a five-second cleanup window before forced termination;
- machine-scope DPAPI storage for the durable device credential and unattended Access Code;
- a LocalSystem-owned Named Pipe authentication broker bound to the exact Agent PID and user SID;
- short-lived Signal Token exchange/cache without exposing the durable device credential to the user-session Agent.

This provides unattended persistence for a usable interactive user session and reliable recovery when users log in/out or move between console/RDP sessions. Windows sign-in screen and UAC Secure Desktop control remain outside the current trust boundary.

## Build outputs

```powershell
cmake -S apps/windows-agent -B build/windows-agent -A x64
cmake --build build/windows-agent --config Release --parallel
```

The Release directory contains the user-facing manager and host processes:

```text
DeskLink.exe
desklink-agent.exe
desklink-service.exe
desklink-media-probe.exe
```

`DeskLink.exe` is the recommended configuration/install entry point for normal users.

## Configure non-secret settings

Current releases store non-secret host settings in the Windows Service's own registry `Environment` value:

```text
HKLM\SYSTEM\CurrentControlSet\Services\DeskLink\Environment
```

This is preferred over the old v1.0.0-style machine-wide environment variables because the Service Control Manager applies the Service environment on the next Service start/restart without requiring a Windows reboot.

The recommended methods are:

1. run `DeskLink.exe` as administrator; or
2. use `packaging/windows/install-service.ps1` for automated deployment.

Example automated installation:

```powershell
.\install-service.ps1 `
  -SignalUrl "wss://control.example.com/ws" `
  -DeviceId "win-office-01" `
  -SignalTokenUrl "https://control.example.com/api/v1/signal-token" `
  -StunUrl "stun:turn.example.com:3478" `
  -TurnHost "turn.example.com" `
  -TurnCredentialsUrl "https://control.example.com/api/v1/turn-credentials" `
  -Fps 60
```

High-refresh hosts may configure `-Fps 90`, `120` or `144`. The accepted range is 15–144. Higher configured FPS is a target, not a guarantee: the Agent can fall back to a lower compatible high-refresh tier when the GPU/Media Foundation encoder cannot initialize at the requested rate.

`-BitrateBps` can override the FPS-aware default bitrate when explicit capacity planning is needed.

## Store the durable device credential

Prefer a random `dc2` generated from the server-side per-device registry. Legacy `dc1` remains accepted for migration.

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

The current validator accepts 8–256 printable characters, but production deployments should use a substantially longer random value because the current network proof is HMAC-based rather than a PAKE.

## Runtime secret boundary

When protected device credential storage exists:

- the Service removes inherited `DESKLINK_DEVICE_CREDENTIAL` from the child environment;
- the durable credential remains in LocalSystem memory;
- the Agent asks the local broker for a short-lived Signal Token;
- the broker performs the HTTPS `/api/v1/signal-token` exchange and caches the result with an expiry safety margin;
- the Agent receives only the short-lived token and expiry.

When protected Access Code storage exists:

- the Service removes inherited `DESKLINK_ACCESS_CODE` from the child environment;
- the Service starts the broker with the `access-code` capability;
- the Agent obtains the Access Code from the local broker during startup;
- if the capability is declared but the broker cannot supply the value, Agent startup fails closed instead of falling back to stale plaintext configuration.

The two broker capabilities are independent so deployments can migrate identity and Access Code separately.

## Named Pipe hardening

The Service creates a unique local-only pipe for each Agent launch. The broker:

- creates the first pipe instance before the suspended Agent is resumed;
- builds an ACL for LocalSystem plus the exact Agent user SID;
- verifies the connecting Named Pipe client PID against the exact Agent process ID;
- supports only the declared capabilities (`signal-token`, `access-code`);
- rejects wrong-PID clients even under the same user account.

The protected Agent process is created suspended in broker mode, assigned to its Job Object, the broker is made ready, and only then is the Agent thread resumed.

## Active session selection

The Service no longer assumes that the only usable desktop is `WTSGetActiveConsoleSessionId()`.

Selection behavior is:

1. enumerate Terminal Services sessions;
2. if the active console session has a real user and is `WTSActive`, prefer it;
3. otherwise select an active interactive user session, which covers RDP-only/Windows Server scenarios;
4. if Terminal Services enumeration itself fails, fall back conservatively to the historical console session only when it has a real user;
5. when no usable interactive user exists, keep the Service online and wait instead of launching the Agent into Session 0.

Session-change notifications wake the supervisor loop immediately. When the selected session changes, the old Agent is stopped/cleaned up and a new Agent is started in the new session.

## Lock/unlock and desktop-transition video recovery

Locking Windows, Fast User Switching, display-mode changes and some desktop transitions can invalidate DXGI Desktop Duplication with `DXGI_ERROR_ACCESS_LOST`.

A previous failure mode was that if immediate recreation failed while Windows was still transitioning, `duplication_` stayed null and future capture calls never retried. The current Agent retains its D3D11 device/adapter and retries Desktop Duplication creation with bounded backoff. Once the normal interactive desktop becomes accessible again, video can resume without requiring a manual Service/Agent restart.

This recovery is specifically for returning to a normal accessible desktop. It does **not** capture the protected UAC Secure Desktop or Windows sign-in desktop.

## Input cleanup during stop/session/network loss

DeskLink tracks synthetic keys and mouse buttons so a remote disconnect does not leave Ctrl/Alt/Shift/Win or a mouse button logically held.

Cleanup boundaries include:

- browser blur/hidden/disconnect (`release-all`);
- input DataChannel closure;
- WebRTC transport loss/closure;
- device revocation;
- accepted replacement session;
- Service stop/session replacement/Agent shutdown.

Key/button injection and pressed-state bookkeeping are serialized. A failed `KEYUP`/`MOUSEUP` is left tracked rather than forgotten, allowing a later cleanup path to retry after a temporary UIPI/desktop boundary clears. High-frequency pointer movement remains outside this lock.

## Install and restart

For the graphical path, extract the full Windows release package and run:

```text
DeskLink.exe
```

For automation, run the packaged installer script from an elevated PowerShell window. Direct Service installation remains available:

```powershell
.\desklink-service.exe --install
Restart-Service DeskLink
```

The Service is configured for automatic start and restart failure actions.

## Graceful shutdown

On Service stop, session replacement or planned Agent restart:

1. the Service signals the unique Agent stop event;
2. the Agent releases remotely-held keys/buttons;
3. signaling, PeerConnection, encoder, GPU conversion and capture are closed normally;
4. the Service waits up to five seconds;
5. only if the Agent does not exit does the Service terminate the Job/process.

This avoids making forced process termination the normal cleanup path.

## Crash-loop behavior

Unexpected Agent exits use exponential relaunch backoff. A sufficiently stable run resets the failure streak. Session changes reset prior Agent launch state so a new legitimate session is not delayed by failures from the previous session.

## Diagnostics

`DeskLink.exe` provides configuration/network/service diagnostics. The Windows package also includes:

```text
desklink-media-probe.exe
```

The probe exercises the local D3D11/DXGI/GPU BGRA→NV12/Media Foundation H.264 path without opening a remote session. Use it when control works but video does not.

## Remove protected secrets

```powershell
.\desklink-service.exe --clear-access-code
.\desklink-service.exe --clear-device-credential
```

After a successful DPAPI migration, remove any historical plaintext settings left by older deployments and restart the Service. Current installers already clean v1.0.0 legacy non-secret machine environment settings.

If a protected DPAPI file exists but is corrupt/unreadable, Service/Agent launch fails closed for that capability; it does not silently recover using a stale plaintext secret.

## Uninstall

```powershell
.\desklink-service.exe --uninstall
```

Uninstalling the Service does not automatically delete `%ProgramData%\DeskLink` protected secret files. Clear protected secrets explicitly when decommissioning a machine, or use the packaged uninstall script options that remove them intentionally.

## Remaining privileged-session limitation

The current Service deliberately leaves capture and ordinary input in the active user session. It therefore does not yet provide reliable control of:

- the Windows sign-in screen;
- UAC Secure Desktop consent UI;
- other desktops isolated from the normal user desktop;
- Ctrl+Alt+Del/SAS through an implemented privileged DeskLink broker.

This must not be solved by disabling UAC or lowering Windows security policy. The intended direction is a minimal LocalSystem privileged desktop/system-operation broker with narrowly scoped authenticated IPC to the normal Agent. The full WebRTC/network/media process should remain outside that privileged component.
