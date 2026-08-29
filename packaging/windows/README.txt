DeskLink Windows Host
=====================

This package contains:

  desklink-agent.exe
  desklink-service.exe
  install-service.ps1
  uninstall-service.ps1

Recommended production installation
-----------------------------------

1. Open PowerShell as Administrator in this directory.
2. Install binaries and NON-SECRET configuration. Example:

   .\install-service.ps1 `
     -SignalUrl "wss://control.example.com/ws" `
     -DeviceId "office-pc-01" `
     -SignalTokenUrl "https://control.example.com/api/v1/signal-token" `
     -StunUrl "stun:turn.example.com:3478" `
     -TurnHost "turn.example.com" `
     -TurnCredentialsUrl "https://control.example.com/api/v1/turn-credentials"

3. Provision the long-lived per-device credential. The Service prompts with console
   echo disabled and stores it using machine-scope DPAPI:

   & "$env:ProgramFiles\DeskLink\desklink-service.exe" --store-device-credential

4. Provision the unattended Access Code the same way:

   & "$env:ProgramFiles\DeskLink\desklink-service.exe" --store-access-code

5. Restart the Service:

   Restart-Service DeskLink

Security notes
--------------

- Do not put DESKLINK_DEVICE_CREDENTIAL or DESKLINK_ACCESS_CODE in machine
  environment variables for unattended production hosts.
- Runtime media/control uses WebRTC encryption; signaling should use WSS and TURN
  credentials should be short-lived.
- The current Service supports the active logged-in console session. Windows
  Winlogon/UAC Secure Desktop control is not implemented in v1.0.0.
- v1.0.0 binaries are not Authenticode-signed. Windows SmartScreen may warn until
  a future signed installer is shipped.

Uninstall
---------

  .\uninstall-service.ps1

To permanently remove the protected device identity as well:

  .\uninstall-service.ps1 -RemoveProtectedSecrets -RemoveFiles
