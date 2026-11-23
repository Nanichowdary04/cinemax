<#
.SYNOPSIS
  One-shot build for CINEMAX: UI bundle -> CMake/MSVC build -> tests -> dist folder
  (-> installer with -Installer when Inno Setup is installed).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\build.ps1
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -Installer
#>
param(
  [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
  [string]$Config = 'Release',
  [string]$DevRoot = (Join-Path $env:USERPROFILE 'CINEMAX-dev'),
  [switch]$SkipUi,
  [switch]$SkipTests,
  [switch]$Installer
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $DevRoot "build\$Config"
$distDir = Join-Path $root 'dist\CINEMAX'

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "build: ERROR: $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
Step 'Locating Visual Studio C++ toolchain'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found - install Visual Studio Build Tools with "Desktop development with C++".' }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { Fail 'No Visual Studio installation with the MSVC x64 toolset was found.' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
Write-Host "  $vsPath"

# Import the vcvars64 environment into this PowerShell session.
$envDump = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail 'cl.exe not available after vcvars64.bat.' }

# Prefer the CMake/Ninja that ship with Visual Studio.
$vsCmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
if (Test-Path $vsCmake) {
  $env:PATH = "$vsCmake\CMake\bin;$vsCmake\Ninja;$env:PATH"
}

# ---------------------------------------------------------------------------
$uiHtml = Join-Path $DevRoot 'ui-build\dist\index.html'
if (-not $SkipUi) {
  Step 'Building the React UI'
  $out = & (Join-Path $root 'ui\build-ui.ps1') -StageDir (Join-Path $DevRoot 'ui-build')
  if ($LASTEXITCODE -ne 0) { Fail 'UI build failed.' }
  $uiHtml = ($out | Select-Object -Last 1).Trim()
}
if (-not (Test-Path $uiHtml)) { Fail "UI bundle not found at $uiHtml (run without -SkipUi)." }

# ---------------------------------------------------------------------------
Step "Configuring CMake ($Config)"
$juceDir = Join-Path $DevRoot 'deps\JUCE'
$cmakeArgs = @('-S', $root, '-B', $buildDir, '-G', 'Ninja',
               "-DCMAKE_BUILD_TYPE=$Config",
               '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
               "-DCINEMAX_UI_HTML=$uiHtml")
if (Test-Path (Join-Path $juceDir 'CMakeLists.txt')) { $cmakeArgs += "-DCINEMAX_JUCE_DIR=$juceDir" }
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed.' }

Step 'Building'
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { Fail 'Build failed.' }

$appExe   = Get-ChildItem -Path $buildDir -Recurse -Filter 'CINEMAX.exe' | Where-Object { $_.FullName -match 'Cinemax_artefacts' } | Select-Object -First 1
$guardExe = Get-ChildItem -Path $buildDir -Recurse -Filter 'CinemaxGuard.exe' | Select-Object -First 1
$cliExe   = Get-ChildItem -Path $buildDir -Recurse -Filter 'cinemax_cli.exe' | Select-Object -First 1
$testExe  = Get-ChildItem -Path $buildDir -Recurse -Filter 'cinemax_tests.exe' | Select-Object -First 1
if (-not $appExe -or -not $guardExe) { Fail 'Build outputs not found.' }

# ---------------------------------------------------------------------------
if (-not $SkipTests) {
  Step 'Running DSP tests'
  & $testExe.FullName
  if ($LASTEXITCODE -ne 0) { Fail 'DSP tests failed.' }
}

# ---------------------------------------------------------------------------
Step "Staging $distDir"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Copy-Item $appExe.FullName, $guardExe.FullName -Destination $distDir -Force
if ($cliExe) { Copy-Item $cliExe.FullName -Destination (Join-Path $distDir 'cinemax-cli.exe') -Force }
# Webcam head tracking: ONNX Runtime (official NuGet package, pinned) + the BlazeFace model
$ortVersion = '1.32.0'
$ortDir = Join-Path $DevRoot "deps\onnxruntime-$ortVersion"
$ortDll = Join-Path $ortDir 'runtimes\win-x64\native\onnxruntime.dll'
if (-not (Test-Path $ortDll)) {
  Step "Downloading ONNX Runtime $ortVersion (NuGet)"
  New-Item -ItemType Directory -Force -Path $ortDir | Out-Null
  $pkg = Join-Path $ortDir 'pkg.zip'
  Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/$ortVersion" -OutFile $pkg -UseBasicParsing
  Expand-Archive -Path $pkg -DestinationPath $ortDir -Force
  Remove-Item $pkg
}
Copy-Item $ortDll -Destination $distDir -Force
Copy-Item (Join-Path $ortDir 'runtimes\win-x64\native\onnxruntime_providers_shared.dll') -Destination $distDir -Force
$modelsDst = Join-Path $distDir 'models'
New-Item -ItemType Directory -Force -Path $modelsDst | Out-Null
Copy-Item (Join-Path $root 'data\models\*') -Destination $modelsDst -Force
Copy-Item (Join-Path $ortDir 'LICENSE') -Destination (Join-Path $modelsDst 'ONNXRUNTIME-LICENSE.txt') -Force
Copy-Item (Join-Path $ortDir 'ThirdPartyNotices.txt') -Destination (Join-Path $modelsDst 'ONNXRUNTIME-ThirdPartyNotices.txt') -Force

# Real-ear sets (measured HRTFs) for headphone 3D sound
$hrtfDst = Join-Path $distDir 'hrtf'
New-Item -ItemType Directory -Force -Path $hrtfDst | Out-Null
Copy-Item (Join-Path $root 'data\hrtf\*') -Destination $hrtfDst -Force
foreach ($doc in @('README.md', 'docs\TROUBLESHOOTING.md', 'docs\RECOVERY.md', 'docs\VISION-ENGINE.md', 'LICENSE-NOTICE.md')) {
  $p = Join-Path $root $doc
  if (Test-Path $p) { Copy-Item $p -Destination $distDir -Force }
}
Get-ChildItem $distDir | Select-Object Name, @{ n = 'KB'; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize

# ---------------------------------------------------------------------------
if ($Installer) {
  Step 'Building installer (Inno Setup)'
  $iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
  if (-not $iscc) {
    foreach ($c in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                     "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
      if (Test-Path $c) { $iscc = $c; break }
    }
  }
  if (-not $iscc) { Fail 'Inno Setup 6 (ISCC.exe) not found. Install it (winget install JRSoftware.InnoSetup) or skip -Installer.' }
  & $iscc "/DAppVersion=1.0.0" "/DSourceDir=$distDir" "/O$(Join-Path $root 'dist')" (Join-Path $root 'installer\cinemax.iss')
  if ($LASTEXITCODE -ne 0) { Fail 'Installer build failed.' }
}

Write-Host "`nBuild complete: $distDir" -ForegroundColor Green
