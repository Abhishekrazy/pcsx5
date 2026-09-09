param(
    [Parameter(Mandatory=$true)][string]$SdkRoot,
    [Parameter(Mandatory=$true)][string]$NdkRoot,
    [Parameter(Mandatory=$true)][string]$JdkRoot,
    [string]$BuildToolsVersion='36.0.0',
    [string]$Platform='android-37.0',
    [switch]$DeviceTests
)
$ErrorActionPreference='Stop'
function Check-Native([string]$Step) { if ($LASTEXITCODE -ne 0) { throw "$Step failed ($LASTEXITCODE)." } }
$projectRoot=Split-Path $PSScriptRoot -Parent
$buildTools=Join-Path $SdkRoot "build-tools/$BuildToolsVersion"
$androidJar=Join-Path $SdkRoot "platforms/$Platform/android.jar"
foreach ($path in @($androidJar,(Join-Path $buildTools 'aapt.exe'),(Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'))) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required installed tool is missing: $path" }
}
Push-Location $projectRoot
try {
    $native='out/build/android-app-release'
    cmake -S . -B $native -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    Check-Native 'Native configure'
    cmake --build $native --target pcsx5_app --parallel 2
    Check-Native 'Native build'
    $output=Join-Path $projectRoot ('out/android-app-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path "$output/classes","$output/dex","$output/package/lib/arm64-v8a","$output/package/assets" -Force | Out-Null
    Copy-Item -LiteralPath 'LICENSE' -Destination "$output/package/assets/LICENSE"
    Copy-Item -LiteralPath "$NdkRoot/NOTICE" -Destination "$output/package/assets/NDK-NOTICE"
    Copy-Item -LiteralPath "$NdkRoot/NOTICE.toolchain" -Destination "$output/package/assets/NDK-TOOLCHAIN-NOTICE"
    $javaSources=@('frontend/android/MainActivity.java')
    $manifest='frontend/android/AndroidManifest.xml'
    if ($DeviceTests) {
        $javaSources+='frontend/android/DeviceAcceptance.java'
        [xml]$testManifest=Get-Content -LiteralPath $manifest -Raw
        $instrumentation=$testManifest.CreateElement('instrumentation')
        $instrumentation.SetAttribute('name','http://schemas.android.com/apk/res/android','org.pcsx5.experimental.DeviceAcceptance') | Out-Null
        $instrumentation.SetAttribute('targetPackage','http://schemas.android.com/apk/res/android','org.pcsx5.experimental') | Out-Null
        $testManifest.manifest.AppendChild($instrumentation) | Out-Null
        $manifest="$output/AndroidManifest.xml"
        $testManifest.Save($manifest)
    }
    & "$JdkRoot/bin/javac.exe" -source 8 -target 8 -Xlint:-options -bootclasspath $androidJar -d "$output/classes" @javaSources
    Check-Native 'Java compile'
    $classes=@(Get-ChildItem -LiteralPath "$output/classes" -Recurse -Filter '*.class' | ForEach-Object FullName)
    & "$buildTools/d8.bat" --min-api 26 --lib $androidJar --output "$output/dex" @classes
    Check-Native 'Dex compile'
    Copy-Item -LiteralPath "$output/dex/classes.dex" -Destination "$output/package/classes.dex"
    Copy-Item -LiteralPath "$native/frontend/libpcsx5_app.so" -Destination "$output/package/lib/arm64-v8a/libpcsx5_app.so"
    & "$buildTools/aapt.exe" package -f -M $manifest -I $androidJar -F "$output/unsigned.apk" "$output/package"
    Check-Native 'APK package'
    & "$buildTools/zipalign.exe" -P 16 -f 4 "$output/unsigned.apk" "$output/aligned.apk"
    Check-Native 'APK align'
    # Public development password, a local disposable test identity; never release signing.
    & "$JdkRoot/bin/keytool.exe" -genkeypair -keystore "$output/developer.keystore" -storepass android -keypass android -alias pcsx5-debug -keyalg RSA -keysize 2048 -validity 3650 -dname 'CN=PCSX5 Experimental Development' -noprompt
    Check-Native 'Development key generation'
    & "$buildTools/apksigner.bat" sign --ks "$output/developer.keystore" --ks-key-alias pcsx5-debug --ks-pass pass:android --key-pass pass:android --out "$output/pcsx5-experimental.apk" "$output/aligned.apk"
    Check-Native 'Development signing'
    & "$buildTools/apksigner.bat" verify --verbose "$output/pcsx5-experimental.apk"
    Check-Native 'APK verification'
    & "$buildTools/aapt.exe" dump badging "$output/pcsx5-experimental.apk"
    Check-Native 'APK manifest verification'
    Write-Host "Developer APK ready: $output/pcsx5-experimental.apk (not installed; not a release)."
} finally { Pop-Location }
