[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$BinaryDirectory,
  [string]$CertificateBase64 = $env:DESKLINK_WINDOWS_SIGNING_CERT_BASE64,
  [string]$CertificatePassword = $env:DESKLINK_WINDOWS_SIGNING_CERT_PASSWORD,
  [string]$TimestampUrl = $env:DESKLINK_WINDOWS_SIGNING_TIMESTAMP_URL,
  [switch]$RequireSignature,
  [switch]$VerifyOnly
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $BinaryDirectory)) {
  throw "Binary directory does not exist: $BinaryDirectory"
}

$targets = @(
  (Join-Path $BinaryDirectory "DeskLink.exe"),
  (Join-Path $BinaryDirectory "desklink-agent.exe"),
  (Join-Path $BinaryDirectory "desklink-service.exe"),
  (Join-Path $BinaryDirectory "desklink-media-probe.exe")
)
foreach ($target in $targets) {
  if (-not (Test-Path $target)) { throw "Signing target is missing: $target" }
}

$kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$signtool = Get-ChildItem "$kitsRoot\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
  Sort-Object FullName |
  Select-Object -Last 1
if (-not $signtool) { throw "signtool.exe was not found in the Windows SDK." }

function Verify-DeskLinkSignature([string]$Target) {
  & $signtool.FullName verify /pa /all /v $Target
  if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $Target" }
}

if ($VerifyOnly) {
  foreach ($target in $targets) {
    Write-Host "Verifying Authenticode signature for $(Split-Path $target -Leaf)"
    Verify-DeskLinkSignature $target
  }
  exit 0
}

if ([string]::IsNullOrWhiteSpace($CertificateBase64)) {
  if ($RequireSignature) {
    throw "Authenticode certificate is required for this release but DESKLINK_WINDOWS_SIGNING_CERT_BASE64 is not configured."
  }
  Write-Warning "Authenticode certificate is not configured. Development Windows binaries will remain unsigned."
  Write-Host "Configure DESKLINK_WINDOWS_SIGNING_CERT_BASE64 and DESKLINK_WINDOWS_SIGNING_CERT_PASSWORD for signed builds."
  exit 0
}

if ([string]::IsNullOrWhiteSpace($CertificatePassword)) {
  throw "A signing certificate was provided but DESKLINK_WINDOWS_SIGNING_CERT_PASSWORD is empty."
}
if ([string]::IsNullOrWhiteSpace($TimestampUrl)) {
  $TimestampUrl = "http://timestamp.digicert.com"
}

$pfxPath = Join-Path $env:RUNNER_TEMP ("desklink-signing-{0}.pfx" -f [Guid]::NewGuid().ToString("N"))
$imported = $null
try {
  [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($CertificateBase64))
  $securePassword = ConvertTo-SecureString $CertificatePassword -AsPlainText -Force
  $imported = Import-PfxCertificate `
    -FilePath $pfxPath `
    -CertStoreLocation Cert:\CurrentUser\My `
    -Password $securePassword `
    -Exportable:$false
  if (-not $imported) { throw "Unable to import Authenticode certificate." }

  foreach ($target in $targets) {
    Write-Host "Signing $(Split-Path $target -Leaf) with certificate $($imported.Thumbprint)"
    & $signtool.FullName sign `
      /sha1 $imported.Thumbprint `
      /fd SHA256 `
      /tr $TimestampUrl `
      /td SHA256 `
      /v `
      $target
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $target" }

    Verify-DeskLinkSignature $target
  }
} finally {
  if ($imported) {
    Remove-Item ("Cert:\CurrentUser\My\" + $imported.Thumbprint) -Force -ErrorAction SilentlyContinue
  }
  if (Test-Path $pfxPath) {
    Remove-Item $pfxPath -Force -ErrorAction SilentlyContinue
  }
}
