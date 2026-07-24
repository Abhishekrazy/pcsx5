<#
.SYNOPSIS
    PCSX5 Release Build Script - builds C++ core + C# WPF UI and stages to dist\

.PARAMETER OutputDir
    Target directory for staged output (default: dist\)
.PARAMETER Clean
    Remove build/ and dist/ before building
.PARAMETER SkipCpp
    Skip the CMake C++ core build
.PARAMETER SkipDotnet
    Skip the dotnet WPF UI publish
.PARAMETER Zip
    Create a release zip archive after staging
.PARAMETER Version
    Version string embedded in the zip name (e.g. "0.1.0")
#>
param(
    [string] $OutputDir  = "dist",
    [switch] $Clean,
    [switch] $SkipCpp,
    [switch] $SkipDotnet,
    [switch] $Zip,
    [string] $Version    = "",
    [string] $Generator  = ""    # empty = auto-detect VS; set to "Ninja" for Ninja
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Log   { param([string]$m, [string]$l = "INFO")  Write-Host "[$l] $m" }
function Warn  { param([string]$m) Log $m "WARN" }
function Fatal { param([string]$m) Log $m "ERROR"; exit 1 }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoRoot  = Resolve-Path $scriptDir
if (-not (Test-Path (Join-Path $repoRoot "CMakeLists.txt"))) {
    Fatal "Cannot find CMakeLists.txt at repo root: $repoRoot"
}

$buildDir  = Join-Path $repoRoot "build"
$distDir   = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $repoRoot $OutputDir }
$assetsDir = Join-Path $repoRoot "assets"

# Clean
if ($Clean) {
    foreach ($d in @($buildDir, $distDir)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d; Log "Removed $d" }
    }
}

New-Item -ItemType Directory $distDir -Force -ErrorAction SilentlyContinue | Out-Null

# ---------------------------------------------------------------------------
# Step 1 - C++ core (CMake + Ninja)
# ---------------------------------------------------------------------------
if (-not $SkipCpp) {
    # Detect the best generator: user override > existing cache > VS auto-detect.
    $actualGen = ""
    if ($Generator) {
        $actualGen = $Generator
    } else {
        $cacheFile = Join-Path $buildDir "CMakeCache.txt"
        if (Test-Path $cacheFile) {
            $existingGen = (Select-String -Path $cacheFile -Pattern "^CMAKE_GENERATOR:INTERNAL=(.+)$").Matches.Groups[1].Value
            if ($existingGen) { $actualGen = $existingGen }
        }
        if (-not $actualGen) {
            # Auto-detect Visual Studio via vswhere.
            $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            if (Test-Path $vswhere) {
                $vsInfo = & $vswhere -format json -latest | ConvertFrom-Json
                if ($vsInfo) {
                    $ver = $vsInfo[0].installationVersion
                    $majorVer = $ver -replace '\..*', ''
                    # Map major version to year: VS 16=2019, 17=2022, 18=2026
                    $yearMap = @{ "16" = "2019"; "17" = "2022"; "18" = "2026" }
                    $year = if ($yearMap.ContainsKey($majorVer)) { $yearMap[$majorVer] } else { $majorVer }
                    $actualGen = "Visual Studio $majorVer $year"
                    Log "Auto-detected: $actualGen (version $ver)"
                }
            }
            if (-not $actualGen) { $actualGen = "Visual Studio 17 2022" }  # fallback
        }
    }

    Log "=== Step 1: CMake configure ($actualGen) ==="
    & cmake -B $buildDir -S $repoRoot -G $actualGen -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { Fatal "CMake configure failed (exit $LASTEXITCODE)" }

    Log "=== Step 1b: CMake build (Release) ==="
    & cmake --build $buildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Fatal "CMake build failed (exit $LASTEXITCODE)" }

    # CMake sets RUNTIME_OUTPUT_DIRECTORY = build/bin.
    # With Visual Studio generators the config name is appended (bin/Release/).
    $cppBinDir = Join-Path $buildDir "bin"
    if (-not (Test-Path (Join-Path $cppBinDir "pcsx5_core.dll"))) {
        $cppBinDir = Join-Path $buildDir "bin\Release"
    }
    if (-not (Test-Path (Join-Path $cppBinDir "pcsx5_core.dll"))) {
        $cppBinDir = Join-Path $buildDir "Release"
    }
    if (-not (Test-Path (Join-Path $cppBinDir "pcsx5_core.dll"))) {
        $cppBinDir = $buildDir
    }
    Log "C++ binaries in: $cppBinDir"
} else {
    $cppBinDir = Join-Path $buildDir "bin"
    if (-not (Test-Path $cppBinDir)) { $cppBinDir = $buildDir }
}

# ---------------------------------------------------------------------------
# Step 2 - C# WPF frontend (dotnet publish)
# ---------------------------------------------------------------------------
$publishDir = Join-Path $buildDir "publish"
if (-not $SkipDotnet) {
    $csprojPath = Join-Path $repoRoot "src\ui_csharp\Pcsx5Ui.csproj"
    Log "=== Step 2: dotnet publish (Release, win-x64) ==="
    & dotnet publish $csprojPath -c Release -r win-x64 --self-contained true `
        -p:PublishSingleFile=true `
        -p:IncludeNativeLibrariesForSelfExtract=true `
        -o $publishDir
    if ($LASTEXITCODE -ne 0) { Fatal "dotnet publish failed (exit $LASTEXITCODE)" }
}

# ---------------------------------------------------------------------------
# Step 3 - Stage to dist\
# ---------------------------------------------------------------------------
Log "=== Step 3: Staging dist\ ==="

function Stage-File {
    param([string]$Src, [string]$Dst)
    if (Test-Path $Src) {
        $d = Split-Path -Parent $Dst
        if (-not (Test-Path $d)) { New-Item -ItemType Directory $d -Force | Out-Null }
        Copy-Item -Path $Src -Destination $Dst -Force
        Log "  + $(Split-Path $Dst -Leaf)"
        return $true
    } else {
        Warn "  MISSING: $Src"
        return $false
    }
}
function Stage-Dir {
    param([string]$Src, [string]$Dst)
    if (Test-Path $Src) {
        Copy-Item -Path $Src -Destination $Dst -Recurse -Force
        Log "  + $(Split-Path $Dst -Leaf)\ (dir)"
    }
}

# --- dist layout:
#   pcsx5.exe          — WPF launcher (root)
#   pcsx5_cli.exe      — CLI runner (root)
#   plugins/pcsx5_core.dll, av*.dll
#   tools/pcsx5_snd_decode.exe, pcsx5_boot_parser.exe
#   assets/nid_db.txt, lang/
#   pcsx5_config/global.json
#   pcsx5_crash/, pcsx5_savedata/, Cache/

# Root — launchers
$wpfExe = Join-Path $publishDir "pcsx5.exe"
if (Test-Path $wpfExe) {
    Stage-File $wpfExe (Join-Path $distDir "pcsx5.exe")
} elseif (-not $SkipDotnet) {
    Fatal "pcsx5.exe not found in $publishDir"
}
Stage-File (Join-Path $cppBinDir "pcsx5_cli.exe")  (Join-Path $distDir "pcsx5_cli.exe")

# Plugins — core DLL + FFmpeg
$distPlugins = Join-Path $distDir "plugins"
New-Item $distPlugins -Force -Type Directory | Out-Null
Stage-File (Join-Path $cppBinDir "pcsx5_core.dll") (Join-Path $distPlugins "pcsx5_core.dll")
foreach ($pDir in @("I:\InstalledGames\sharpemu-win64-fbf2c2d\plugins",".\sharpemu_plugins","..\sharpemu_plugins")) {
    if (Test-Path $pDir) {
        foreach ($dll in @("avformat-61.dll","avcodec-61.dll","avutil-59.dll","swscale-8.dll","swresample-5.dll")) {
            $src = Join-Path $pDir $dll
            if (Test-Path $src) { Stage-File $src (Join-Path $distPlugins $dll) }
        }
    }
}

# Tools
$distTools = Join-Path $distDir "tools"
New-Item $distTools -Force -Type Directory | Out-Null
Stage-File (Join-Path $cppBinDir "pcsx5_snd_decode.exe") (Join-Path $distTools "pcsx5_snd_decode.exe")
$bp = Join-Path $buildDir "Release\pcsx5_boot_parser.exe"
if (-not (Test-Path $bp)) { $bp = Join-Path $cppBinDir "pcsx5_boot_parser.exe" }
if (-not (Test-Path $bp)) { $bp = Join-Path $buildDir "pcsx5_boot_parser.exe" }
Stage-File $bp (Join-Path $distTools "pcsx5_boot_parser.exe")

# Assets
$distAssets = Join-Path $distDir "assets"
New-Item $distAssets -Force -Type Directory | Out-Null
Stage-File (Join-Path $assetsDir "nid_db.txt") (Join-Path $distAssets "nid_db.txt")
Stage-Dir (Join-Path $assetsDir "lang")        (Join-Path $distDir "lang")
$lua = Join-Path $assetsDir "pcsx5_init.lua"
if (Test-Path $lua) { Stage-File $lua (Join-Path $distAssets "pcsx5_init.lua") }

# Config
$distConfigDir = Join-Path $distDir "pcsx5_config"
$srcGlobal = Join-Path $repoRoot "pcsx5_config\global.json"
if (Test-Path $srcGlobal) {
    Stage-File $srcGlobal (Join-Path $distConfigDir "global.json")
} else {
    New-Item $distConfigDir -Force -Type Directory | Out-Null
    $json = @('{','  "schema_version": 3,','  "logging": { "min_level": "Info" },','  "crash": { "bundle_dir": "pcsx5_crash" },','  "graphics": { "headless": false, "vsync": true },','  "audio": { "backend": 2, "volume": 1.0 },','  "input": { "backend": 0 },','  "loader": { "firmware_modules_dir": "" }','}') -join "`n"
    Set-Content (Join-Path $distConfigDir "global.json") $json -Encoding UTF8
}

# Runtime dirs
foreach ($d in @("pcsx5_crash","pcsx5_savedata","Cache")) { New-Item (Join-Path $distDir $d) -Force -Type Directory | Out-Null }

# ---------------------------------------------------------------------------
# Step 4 - Validation
# ---------------------------------------------------------------------------
Log "=== Step 4: Validating dist\ ==="
$missing = @()
$mandatory = @("pcsx5_core.dll", "pcsx5_cli.exe", "assets\nid_db.txt")
if (-not $SkipDotnet) { $mandatory += "pcsx5.exe" }
foreach ($f in $mandatory) {
    if (-not (Test-Path (Join-Path $distDir $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Warn "MISSING MANDATORY FILES: $($missing -join ', ')"
    Warn "The build may still be usable if you only need CLI mode."
} else {
    Log "All mandatory files present."
}

# ---------------------------------------------------------------------------
# Step 5 - Zip archive (optional)
# ---------------------------------------------------------------------------
if ($Zip) {
    $verSuffix = if ($Version) { "_$Version" } else { "" }
    $zipName = "PCSX5${verSuffix}_Release.zip"
    $zipPath = Join-Path $repoRoot $zipName
    Log "=== Step 5: Creating $zipName ==="
    Compress-Archive -Path (Join-Path $distDir "*") -DestinationPath $zipPath -Force
    Log "Created $zipPath"
}

Log "=== BUILD COMPLETE ==="
Log "Output: $distDir"
if ($Zip) { Log "Archive: $zipPath" }
