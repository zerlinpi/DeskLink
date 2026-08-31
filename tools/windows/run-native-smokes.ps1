param(
  [string]$BuildDirectory = "build/windows-agent",
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$cmakePath = "apps/windows-agent/CMakeLists.txt"
if (-not (Test-Path $cmakePath)) {
  throw "DeskLink Windows CMake file was not found: $cmakePath"
}

$cmake = Get-Content $cmakePath -Raw
$matches = [regex]::Matches(
  $cmake,
  '(?m)add_executable\((desklink-[A-Za-z0-9-]+-smoke)\b'
)
$targets = @(
  $matches |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
)

if ($targets.Count -eq 0) {
  throw "No DeskLink native smoke targets were discovered in $cmakePath"
}

$releaseDirectory = Join-Path $BuildDirectory $Configuration
$executed = 0
foreach ($target in $targets) {
  $executable = Join-Path $releaseDirectory "$target.exe"
  if (-not (Test-Path $executable)) {
    throw "Native smoke target was declared by CMake but not built: $executable"
  }

  Write-Host "Running $target"
  & (Resolve-Path $executable)
  if ($LASTEXITCODE -ne 0) {
    throw "$target failed with exit code $LASTEXITCODE"
  }
  $executed++
}

Write-Host "DeskLink native smoke suite passed: $executed target(s)."
