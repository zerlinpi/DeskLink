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

function Set-MachineEnvironment([string]$Name, [string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    [Environment]::SetEnvironmentVariable($Name, $null, [EnvironmentVariableTarget]::Machine)
  } else {
    [Environment]::SetEnvironmentVariable($Name, $Value, [EnvironmentVariableTarget]::Machine)
  }
}

Assert-Administrator

$packageDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$agentSource = Join-Path $packageDir "desklink-agent.exe"
$serviceSource = Join-Path $packageDir "desklink-service.exe"
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
Copy-Item $MyInvocation.MyCommand.Path (Join-Path $InstallDir "install-service.ps1") -Force
$uninstaller = Join-Path $packageDir "uninstall-service.ps1"
if (Test-Path $uninstaller) {
  Copy-Item $uninstaller (Join-Path $InstallDir "uninstall-service.ps1") -Force
}

Set-MachineEnvironment "DESKLINK_SIGNAL_URL" $SignalUrl
Set-MachineEnvironment "DESKLINK_DEVICE_ID" $DeviceId
Set-MachineEnvironment "DESKLINK_SIGNAL_TOKEN_URL" $SignalTokenUrl
Set-MachineEnvironment "DESKLINK_STUN_URL" $StunUrl
Set-MachineEnvironment "DESKLINK_TURN_HOST" $TurnHost
Set-MachineEnvironment "DESKLINK_TURN_PORT" ([string]$TurnPort)
Set-MachineEnvironment "DESKLINK_TURN_TLS_PORT" ([string]$TurnTlsPort)
Set-MachineEnvironment "DESKLINK_TURN_CREDENTIALS_URL" $TurnCredentialsUrl
Set-MachineEnvironment "DESKLINK_OUTPUT_INDEX" ([string]$OutputIndex)

if (-not [string]::IsNullOrWhiteSpace($SignalTokenUrl)) {
  Set-MachineEnvironment "DESKLINK_SIGNAL_TOKEN_REQUIRED" "1"
}
if (-not [string]::IsNullOrWhiteSpace($TurnCredentialsUrl)) {
  Set-MachineEnvironment "DESKLINK_TURN_RUNTIME_REQUIRED" "1"
}

$serviceExe = Join-Path $InstallDir "desklink-service.exe"
& $serviceExe --install
if ($LASTEXITCODE -ne 0) { throw "DeskLink service installation failed with exit code $LASTEXITCODE." }

Write-Host ""
Write-Host "DeskLink binaries and non-secret machine settings are installed." -ForegroundColor Green
Write-Host "The installer intentionally did NOT put secrets in command-line arguments or machine environment variables."
Write-Host ""
Write-Host "Next, provision the durable device credential and unattended access code from this elevated window:"
Write-Host "  & `"$serviceExe`" --store-device-credential"
Write-Host "  & `"$serviceExe`" --store-access-code"
Write-Host "  Restart-Service DeskLink"
Write-Host ""
Write-Host "The two secrets are stored with machine-scope DPAPI under %ProgramData%\DeskLink."
