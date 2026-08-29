[CmdletBinding()]
param(
  [string]$InstallDir = "$env:ProgramFiles\DeskLink",
  [switch]$RemoveProtectedSecrets,
  [switch]$RemoveFiles
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = [Security.Principal.WindowsPrincipal]::new($identity)
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell window."
  }
}

Assert-Administrator

$serviceExe = Join-Path $InstallDir "desklink-service.exe"
if (Test-Path $serviceExe) {
  if ($RemoveProtectedSecrets) {
    & $serviceExe --clear-access-code
    if ($LASTEXITCODE -ne 0) { throw "Unable to clear protected access code." }
    & $serviceExe --clear-device-credential
    if ($LASTEXITCODE -ne 0) { throw "Unable to clear protected device credential." }
  }

  & $serviceExe --uninstall
  if ($LASTEXITCODE -ne 0) { throw "DeskLink service uninstall failed with exit code $LASTEXITCODE." }
} else {
  $service = Get-Service -Name DeskLink -ErrorAction SilentlyContinue
  if ($service) {
    throw "DeskLink service is registered but desklink-service.exe was not found at $serviceExe. Remove the service with the original binary first."
  }
}

$environmentNames = @(
  "DESKLINK_SIGNAL_URL",
  "DESKLINK_DEVICE_ID",
  "DESKLINK_SIGNAL_TOKEN_URL",
  "DESKLINK_SIGNAL_TOKEN_REQUIRED",
  "DESKLINK_STUN_URL",
  "DESKLINK_TURN_HOST",
  "DESKLINK_TURN_PORT",
  "DESKLINK_TURN_TLS_PORT",
  "DESKLINK_TURN_CREDENTIALS_URL",
  "DESKLINK_TURN_RUNTIME_REQUIRED",
  "DESKLINK_OUTPUT_INDEX"
)
foreach ($name in $environmentNames) {
  [Environment]::SetEnvironmentVariable($name, $null, [EnvironmentVariableTarget]::Machine)
}

if ($RemoveFiles -and (Test-Path $InstallDir)) {
  Remove-Item $InstallDir -Recurse -Force
}

Write-Host "DeskLink service and machine configuration removed." -ForegroundColor Green
if (-not $RemoveProtectedSecrets) {
  Write-Host "Protected DPAPI secrets were retained under %ProgramData%\DeskLink. Use -RemoveProtectedSecrets if you intend to decommission this device identity."
}
