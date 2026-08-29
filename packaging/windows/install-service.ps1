[CmdletBinding()]
param(
  [string]$InstallDir = "$env:ProgramFiles\DeskLink",
  [Parameter(Mandatory = $true)][string]$SignalUrl,
  [Parameter(Mandatory = $true)][string]$DeviceId,
  [string]$SignalTokenUrl = "",
  [string]$StunUrl = "",
  [string]$TurnHost = "",
  [int]$TurnPort = 3478,
  [int]$TurnTlsPort = 5349,
  [string]$TurnCredentialsUrl = "",
  [int]$OutputIndex = 0
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = [Security.Principal.WindowsPrincipal]::new($identity)
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell window."
  }
}

function Add-ServiceEnvironment(
  [System.Collections.Generic.List[string]]$Entries,
  [string]$Name,
  [string]$Value
) {
  if (-not [string]::IsNullOrWhiteSpace($Value)) {
    $Entries.Add("$Name=$Value")
  }
}

Assert-Administrator

if ($SignalUrl -notmatch '^wss?://') {
  throw "SignalUrl must start with ws:// or wss://."
}
if ($DeviceId -notmatch '^[A-Za-z0-9._-]{1,128}$') {
  throw "DeviceId may contain only letters, digits, '.', '_' and '-'."
}
if ($TurnPort -lt 1 -or $TurnPort -gt 65535) { throw "TurnPort must be 1-65535." }
if ($TurnTlsPort -lt 1 -or $TurnTlsPort -gt 65535) { throw "TurnTlsPort must be 1-65535." }
if ($OutputIndex -lt 0 -or $OutputIndex -gt 64) { throw "OutputIndex must be 0-64." }

$packageDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$agentSource = Join-Path $packageDir "desklink-agent.exe"
$serviceSource = Join-Path $packageDir "desklink-service.exe"
$managerSource = Join-Path $packageDir "DeskLink.exe"
if (-not (Test-Path $agentSource)) { throw "desklink-agent.exe was not found beside this installer script." }
if (-not (Test-Path $serviceSource)) { throw "desklink-service.exe was not found beside this installer script." }

$existing = Get-Service -Name DeskLink -ErrorAction SilentlyContinue
if ($existing -and $existing.Status -ne "Stopped") {
  Stop-Service -Name DeskLink -Force
  $existing.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(15))
}

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item $agentSource (Join-Path $InstallDir "desklink-agent.exe") -Force
Copy-Item $serviceSource (Join-Path $InstallDir "desklink-service.exe") -Force
if (Test-Path $managerSource) {
  Copy-Item $managerSource (Join-Path $InstallDir "DeskLink.exe") -Force
}
Copy-Item $MyInvocation.MyCommand.Path (Join-Path $InstallDir "install-service.ps1") -Force
$uninstaller = Join-Path $packageDir "uninstall-service.ps1"
if (Test-Path $uninstaller) {
  Copy-Item $uninstaller (Join-Path $InstallDir "uninstall-service.ps1") -Force
}

$serviceExe = Join-Path $InstallDir "desklink-service.exe"
& $serviceExe --install
if ($LASTEXITCODE -ne 0) { throw "DeskLink service installation failed with exit code $LASTEXITCODE." }

# v1.0.0 used machine-wide environment variables. A Windows service is started
# by SCM and can retain an older environment block until reboot, so newly written
# machine variables are not a reliable configuration channel. Store non-secret
# settings in the service's own REG_MULTI_SZ Environment value instead; SCM reads
# it every time the service process is launched, so Restart-Service applies the
# configuration immediately.
$entries = [System.Collections.Generic.List[string]]::new()
Add-ServiceEnvironment $entries "DESKLINK_SIGNAL_URL" $SignalUrl
Add-ServiceEnvironment $entries "DESKLINK_DEVICE_ID" $DeviceId
Add-ServiceEnvironment $entries "DESKLINK_SIGNAL_TOKEN_URL" $SignalTokenUrl
Add-ServiceEnvironment $entries "DESKLINK_STUN_URL" $StunUrl
Add-ServiceEnvironment $entries "DESKLINK_TURN_HOST" $TurnHost
Add-ServiceEnvironment $entries "DESKLINK_TURN_PORT" ([string]$TurnPort)
Add-ServiceEnvironment $entries "DESKLINK_TURN_TLS_PORT" ([string]$TurnTlsPort)
Add-ServiceEnvironment $entries "DESKLINK_TURN_CREDENTIALS_URL" $TurnCredentialsUrl
Add-ServiceEnvironment $entries "DESKLINK_OUTPUT_INDEX" ([string]$OutputIndex)
if (-not [string]::IsNullOrWhiteSpace($SignalTokenUrl)) {
  Add-ServiceEnvironment $entries "DESKLINK_SIGNAL_TOKEN_REQUIRED" "1"
}
if (-not [string]::IsNullOrWhiteSpace($TurnCredentialsUrl)) {
  Add-ServiceEnvironment $entries "DESKLINK_TURN_RUNTIME_REQUIRED" "1"
}

$serviceRegistry = "HKLM:\SYSTEM\CurrentControlSet\Services\DeskLink"
if (-not (Test-Path $serviceRegistry)) {
  throw "DeskLink service registry key was not created."
}
New-ItemProperty `
  -Path $serviceRegistry `
  -Name Environment `
  -PropertyType MultiString `
  -Value $entries.ToArray() `
  -Force | Out-Null

# Remove v1.0.0 legacy machine-scoped non-secret settings so a manual Agent run
# does not accidentally pick up stale endpoints. Protected secrets were never
# intentionally written here by this installer.
$legacyMachineSettings = @(
  "DESKLINK_SIGNAL_URL",
  "DESKLINK_DEVICE_ID",
  "DESKLINK_SIGNAL_TOKEN_URL",
  "DESKLINK_STUN_URL",
  "DESKLINK_TURN_HOST",
  "DESKLINK_TURN_PORT",
  "DESKLINK_TURN_TLS_PORT",
  "DESKLINK_TURN_CREDENTIALS_URL",
  "DESKLINK_OUTPUT_INDEX",
  "DESKLINK_SIGNAL_TOKEN_REQUIRED",
  "DESKLINK_TURN_RUNTIME_REQUIRED"
)
foreach ($name in $legacyMachineSettings) {
  [Environment]::SetEnvironmentVariable($name, $null, [EnvironmentVariableTarget]::Machine)
}

Restart-Service DeskLink -Force
(Get-Service DeskLink).WaitForStatus("Running", [TimeSpan]::FromSeconds(20))

Write-Host ""
Write-Host "DeskLink binaries and service-scoped configuration are installed." -ForegroundColor Green
Write-Host "Configuration now applies on Service restart; a Windows reboot is not required."
Write-Host "The installer intentionally did NOT put secrets in command-line arguments or environment variables."
Write-Host ""
Write-Host "Next, provision or rotate the durable device credential when SignalTokenUrl is enabled:"
Write-Host "  & `"$serviceExe`" --store-device-credential"
Write-Host ""
Write-Host "Provision the unattended Access Code:"
Write-Host "  & `"$serviceExe`" --store-access-code"
Write-Host "  Restart-Service DeskLink"
Write-Host ""
Write-Host "Both secrets are protected by machine-scope DPAPI under %ProgramData%\DeskLink."
Write-Host "For the easiest setup, run DeskLink.exe from the release package as administrator."
