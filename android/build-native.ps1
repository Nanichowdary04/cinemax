# Builds libcinemax.so (the laptop's CINEMAX DSP engine + Android pipeline) for arm64
# phones with the Android NDK, into %USERPROFILE%\CINEMAX-dev\android-build\jniLibs,
# which app/build.gradle.kts packages into the APK.
#
#   powershell -ExecutionPolicy Bypass -File android\build-native.ps1
param(
    [string]$Ndk = "$env:USERPROFILE\CINEMAX-dev\ndk\android-ndk-r28b",
    [string[]]$Abis = @('arm64-v8a'),
    [string]$Ort = "$env:USERPROFILE\CINEMAX-dev\deps\ort-android"
)
$ErrorActionPreference = 'Stop'

$vsCmake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vsCmake 'CMake\bin\cmake.exe' }
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja) { $ninja = Join-Path $vsCmake 'Ninja\ninja.exe' }
foreach ($t in @($cmake, $ninja, "$Ndk\build\cmake\android.toolchain.cmake")) {
    if (-not (Test-Path $t)) { throw "Missing: $t" }
}

$src = Join-Path $PSScriptRoot 'app\src\main\cpp'
$out = "$env:USERPROFILE\CINEMAX-dev\android-build"
foreach ($abi in $Abis) {
    $bdir = "$out\native\$abi"
    & $cmake -S $src -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
        "-DANDROID_ABI=$abi" '-DANDROID_PLATFORM=android-29' '-DCMAKE_BUILD_TYPE=Release' | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($abi)" }
    & $cmake --build $bdir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($abi)" }
    $dst = "$out\jniLibs\$abi"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item "$bdir\libcinemax.so" $dst -Force
    $strip = Get-ChildItem "$Ndk\toolchains\llvm\prebuilt\*\bin\llvm-strip.exe" | Select-Object -First 1
    if ($strip) { & $strip.FullName --strip-unneeded "$dst\libcinemax.so" }
    "native: $abi OK ($([math]::Round((Get-Item "$dst\libcinemax.so").Length / 1KB)) KB)"

    # Head tracking: ONNX Runtime for Android (from the same NuGet package as the laptop's).
    $ortSo = "$Ort\aar\jni\$abi\libonnxruntime.so"
    if (-not (Test-Path $ortSo)) {
        $nupkg = "$env:USERPROFILE\CINEMAX-dev\deps\ort.nupkg"
        if (-not (Test-Path $nupkg)) { throw "Missing $nupkg (Microsoft.ML.OnnxRuntime 1.32 from NuGet)" }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        New-Item -ItemType Directory -Force "$Ort\aar" | Out-Null
        $zip = [IO.Compression.ZipFile]::OpenRead($nupkg)
        $aarEntry = $zip.Entries | Where-Object FullName -eq 'runtimes/android/native/onnxruntime.aar'
        $aarPath = "$Ort\onnxruntime.aar.zip"   # Expand-Archive only takes .zip names
        [IO.Compression.ZipFileExtensions]::ExtractToFile($aarEntry, $aarPath, $true)
        $zip.Dispose()
        Expand-Archive -Path $aarPath -DestinationPath "$Ort\aar" -Force
    }
    Copy-Item $ortSo $dst -Force
    "native: $abi libonnxruntime.so ($([math]::Round((Get-Item "$dst\libonnxruntime.so").Length / 1MB, 1)) MB)"
}

# Assets: the real-ear recordings and the face model (same files as the laptop).
$repo = Split-Path $PSScriptRoot -Parent
$assets = "$out\assets"
New-Item -ItemType Directory -Force "$assets\hrtf", "$assets\models" | Out-Null
Copy-Item "$repo\data\hrtf\*.chrtf", "$repo\data\hrtf\LICENCES.txt" "$assets\hrtf" -Force
Copy-Item "$repo\data\models\blaze_face_short_range.onnx", "$repo\data\models\NOTICE.txt" "$assets\models" -Force
$ortDocs = "$env:USERPROFILE\CINEMAX-dev\deps\onnxruntime-1.32.0"
if (Test-Path "$ortDocs\LICENSE") {
    Copy-Item "$ortDocs\LICENSE" "$assets\models\ONNXRUNTIME-LICENSE.txt" -Force
    Copy-Item "$ortDocs\ThirdPartyNotices.txt" "$assets\models\ONNXRUNTIME-ThirdPartyNotices.txt" -Force
}
"assets: $((Get-ChildItem "$assets\hrtf\*.chrtf").Count) real-ear sets, face model"
