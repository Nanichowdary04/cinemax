<#
.SYNOPSIS
  Builds the CINEMAX React UI into a single self-contained index.html.

.DESCRIPTION
  npm never runs inside the (OneDrive-synced) source folder. The ui/ sources are
  mirrored into %USERPROFILE%\CINEMAX-dev\ui-build, dependencies are installed
  there, and the production bundle is built there. The generated
  package-lock.json is copied back into ui/ so builds stay reproducible.

  The LAST line written to the output is the absolute path of dist\index.html.
  Exit code is non-zero on any failure.

.PARAMETER StageDir
  Override the staging directory (default: %USERPROFILE%\CINEMAX-dev\ui-build).

.PARAMETER Harness
  Also produce dev-out\harness.html (layout test page with a stub bridge and a
  "LAYOUT TEST - NOT REAL AUDIO" banner). Never shipped.
#>
param(
  [string]$StageDir = (Join-Path $env:USERPROFILE 'CINEMAX-dev\ui-build'),
  [switch]$Harness
)

$ErrorActionPreference = 'Continue'
$src = $PSScriptRoot

function Fail([string]$msg) {
  Write-Host "build-ui: ERROR: $msg" -ForegroundColor Red
  exit 1
}

if (-not (Get-Command node -ErrorAction SilentlyContinue)) { Fail 'Node.js is not installed or not on PATH (need Node 20.19+ or 22.12+).' }
$npm = (Get-Command npm.cmd -ErrorAction SilentlyContinue).Source
if (-not $npm) { Fail 'npm.cmd not found on PATH.' }

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
Write-Host "build-ui: staging $src -> $StageDir"

# Mirror sources; keep node_modules/dist/dev-out in the stage untouched.
& robocopy $src $StageDir /MIR /XD node_modules dist dev-out /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "robocopy failed with exit code $LASTEXITCODE" }
$global:LASTEXITCODE = 0

Push-Location $StageDir
try {
  # Install dependencies only when package.json / lockfile changed.
  $lock = Join-Path $StageDir 'package-lock.json'
  $stamp = Join-Path $StageDir 'node_modules\.cinemax-install-stamp'
  $hashInput = (Get-FileHash (Join-Path $StageDir 'package.json')).Hash
  if (Test-Path $lock) { $hashInput += (Get-FileHash $lock).Hash }
  $previous = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { '' }

  if ($previous -ne $hashInput -or -not (Test-Path (Join-Path $StageDir 'node_modules'))) {
    if (Test-Path $lock) {
      Write-Host 'build-ui: npm ci'
      & $npm ci --no-audit --no-fund
    } else {
      Write-Host 'build-ui: npm install (no lockfile yet)'
      & $npm install --no-audit --no-fund
    }
    if ($LASTEXITCODE -ne 0) { Fail "npm install failed ($LASTEXITCODE)" }

    # Recompute after install (npm install may have created the lockfile).
    $hashInput = (Get-FileHash (Join-Path $StageDir 'package.json')).Hash
    if (Test-Path $lock) { $hashInput += (Get-FileHash $lock).Hash }
    Set-Content -Path $stamp -Value $hashInput -Encoding ascii
  } else {
    Write-Host 'build-ui: dependencies up to date'
  }

  # Copy the lockfile back so the source tree is reproducible.
  $srcLock = Join-Path $src 'package-lock.json'
  if (Test-Path $lock) {
    if (-not (Test-Path $srcLock) -or (Get-FileHash $srcLock).Hash -ne (Get-FileHash $lock).Hash) {
      Copy-Item $lock $srcLock -Force
      Write-Host 'build-ui: updated ui\package-lock.json'
    }
  }

  Write-Host 'build-ui: npm run build (tsc --noEmit + vite build)'
  & $npm run build
  if ($LASTEXITCODE -ne 0) { Fail "npm run build failed ($LASTEXITCODE)" }

  $out = Join-Path $StageDir 'dist\index.html'
  if (-not (Test-Path $out)) { Fail "build finished but $out is missing" }

  # Sanity check: the bundle must be self-contained (no external script/style/font URLs).
  $html = Get-Content $out -Raw
  if ($html -match '<script[^>]+src=|<link[^>]+href="(?!data:)[^"]+\.(css|js)"|https?://fonts\.') {
    Fail 'dist/index.html references external resources; expected a single self-contained file.'
  }

  if ($Harness) {
    & node (Join-Path $StageDir 'dev\make-harness.mjs')
    if ($LASTEXITCODE -ne 0) { Fail 'harness generation failed' }
  }

  $kb = [math]::Round((Get-Item $out).Length / 1KB, 1)
  Write-Host "build-ui: OK ($kb KB)"
  Write-Output (Resolve-Path $out).Path
}
finally {
  Pop-Location
}
exit 0
