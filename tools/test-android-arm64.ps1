param(
    [Parameter(Mandatory=$true)][string]$NdkRoot,
    [string]$Serial,
    [string]$Adb = 'adb',
    [switch]$BuildOnly,
    [string]$VulkanSdkRoot
)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'An existing Windows Android NDK is required.' }
if (!$BuildOnly -and $Serial -notmatch '^[a-zA-Z0-9._:-]+$') { throw 'An explicit valid device serial is required.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    $remote = '/data/local/tmp/pcsx5-arm64-' + [guid]::NewGuid().ToString('N')
    if (!$BuildOnly) {
        & $Adb -s $Serial shell mkdir $remote
        if ($LASTEXITCODE -ne 0) { throw 'Cannot create isolated device test directory.' }
    }
    New-Item -ItemType Directory -Force -Path 'out/android-arm64' | Out-Null
    $common = @('--target=aarch64-linux-android26','-std=c++23','-O2','-DNDEBUG',
        '-Wall','-Wextra','-Wpedantic','-Wconversion','-static-libstdc++',
        '-Iruntime/include','-Iexecution/include','-Icore/include')
    $cases = @{
        'memory' = @('-DPCSX5_TEST_POSIX=1','tests/runtime_memory_tests.cpp','runtime/src/portable_posix_memory.cpp')
        'services' = @('tests/posix_services_tests.cpp','runtime/src/posix_services.cpp',
            'runtime/src/linux_worker.cpp','runtime/src/linux_timing.cpp')
        'code-cache' = @('tests/executable_code_tests.cpp','runtime/src/posix_executable_code.cpp')
        'execution' = @('tests/arm64_execution_tests.cpp','execution/src/arm64_step.cpp',
            'tests/arm64_host_abi.S',
            'execution/src/arm64_jit.cpp','execution/src/interpreter.cpp',
            'core/src/guest_memory.cpp','runtime/src/posix_executable_code.cpp')
    }
    $names=@('memory','services','code-cache','execution')
    if ($VulkanSdkRoot) {
        $shaderDirectory='out/android-arm64/shaders'
        New-Item -ItemType Directory -Force -Path $shaderDirectory | Out-Null
        foreach ($stage in @('vert','frag')) {
            & "$VulkanSdkRoot/Bin/glslangValidator.exe" -V --target-env vulkan1.1 -o "$shaderDirectory/frame.$stage.spv" "graphics/shaders/frame.$stage"
            if ($LASTEXITCODE -ne 0) { throw 'Shader compilation failed.' }
            & "$VulkanSdkRoot/Bin/spirv-val.exe" --target-env vulkan1.1 "$shaderDirectory/frame.$stage.spv"
            if ($LASTEXITCODE -ne 0) { throw 'Shader validation failed.' }
        }
        cmake "-DVERT=$projectRoot/$shaderDirectory/frame.vert.spv" "-DFRAG=$projectRoot/$shaderDirectory/frame.frag.spv" "-DOUTPUT=$projectRoot/$shaderDirectory/frame_shaders.h" -P cmake/EmbedFrameShaders.cmake
        if ($LASTEXITCODE -ne 0) { throw 'Shader embedding failed.' }
        $cases['graphics']=@('-Igraphics/include',"-I$shaderDirectory",'tests/graphics_vulkan_tests.cpp',
            'graphics/src/renderer.cpp','graphics/src/vulkan_renderer.cpp','-lvulkan')
        $names+='graphics'
    }
    foreach ($name in $names) {
        $output = "out/android-arm64/$name"
        & $compiler @common @($cases[$name]) -o $output
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
        if ($BuildOnly) { continue }
        & $Adb -s $Serial push $output "$remote/$name"
        if ($LASTEXITCODE -ne 0) { throw "Transfer failed: $name" }
        & $Adb -s $Serial shell chmod 700 "$remote/$name"
        if ($LASTEXITCODE -ne 0) { throw "Cannot prepare test: $name" }
        if ($name -eq 'graphics') {
            # System devices do not imply installed validation layers. This
            # functional corpus explicitly reports diagnostics disabled.
            & $Adb -s $Serial shell "$remote/$name" --without-validation
        } else { & $Adb -s $Serial shell "$remote/$name" }
        if ($LASTEXITCODE -ne 0) { throw "Device test failed: $name" }
    }
    if ($BuildOnly) { Write-Host 'Cross-build complete; no device tests executed.' }
    else { Write-Host "PASS. Synthetic binaries retained at $remote; no APK installed." }
} finally { Pop-Location }
