param(
    [Parameter(Mandatory=$true)][string]$NdkRoot,
    [Parameter(Mandatory=$true)][string]$Serial,
    [string]$Adb = 'adb'
)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'An existing Windows Android NDK is required.' }
if ($Serial -notmatch '^[a-zA-Z0-9._:-]+$') { throw 'Invalid device serial.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    $remote = '/data/local/tmp/pcsx5-arm64-' + [guid]::NewGuid().ToString('N')
    & $Adb -s $Serial shell mkdir $remote
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create isolated device test directory.' }
    New-Item -ItemType Directory -Force -Path 'out/android-arm64' | Out-Null
    $common = @('--target=aarch64-linux-android26','-std=c++23','-O2','-DNDEBUG',
        '-Wall','-Wextra','-Wpedantic','-Wconversion','-static-libstdc++',
        '-Iruntime/include','-Iexecution/include','-Icore/include')
    $cases = @{
        'code-cache' = @('tests/executable_code_tests.cpp','runtime/src/posix_executable_code.cpp')
        'execution' = @('tests/arm64_execution_tests.cpp','execution/src/arm64_step.cpp',
            'tests/arm64_host_abi.S',
            'execution/src/arm64_jit.cpp','execution/src/interpreter.cpp',
            'core/src/guest_memory.cpp','runtime/src/posix_executable_code.cpp')
    }
    foreach ($name in @('code-cache','execution')) {
        $output = "out/android-arm64/$name"
        & $compiler @common @($cases[$name]) -o $output
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
        & $Adb -s $Serial push $output "$remote/$name"
        if ($LASTEXITCODE -ne 0) { throw "Transfer failed: $name" }
        & $Adb -s $Serial shell chmod 700 "$remote/$name"
        if ($LASTEXITCODE -ne 0) { throw "Cannot prepare test: $name" }
        & $Adb -s $Serial shell "$remote/$name"
        if ($LASTEXITCODE -ne 0) { throw "Device test failed: $name" }
    }
    Write-Host "PASS. Synthetic binaries retained at $remote; no APK installed."
} finally { Pop-Location }
