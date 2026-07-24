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

# C++ core DLL + CLI tools
Stage-File (Join-Path $cppBinDir "pcsx5_core.dll")         (Join-Path $distDir "pcsx5_core.dll")
Stage-File (Join-Path $cppBinDir "pcsx5_cli.exe")          (Join-Path $distDir "pcsx5_cli.exe")
Stage-File (Join-Path $cppBinDir "pcsx5_snd_decode.exe")   (Join-Path $distDir "pcsx5_snd_decode.exe")
# boot_parser has no RUNTIME_OUTPUT_DIRECTORY, goes to build/Release/
$bp = Join-Path $buildDir "Release\pcsx5_boot_parser.exe"
if (-not (Test-Path $bp)) { $bp = Join-Path $cppBinDir "pcsx5_boot_parser.exe" }
if (-not (Test-Path $bp)) { $bp = Join-Path $buildDir "pcsx5_boot_parser.exe" }
Stage-File $bp (Join-Path $distDir "pcsx5_boot_parser.exe")

# WPF frontend
$wpfExe = Join-Path $publishDir "pcsx5.exe"
if (Test-Path $wpfExe) {
    Stage-File $wpfExe (Join-Path $distDir "pcsx5.exe")
} elseif (-not $SkipDotnet) {
    Fatal "pcsx5.exe not found in $publishDir - dotnet publish may have failed"
}

# Assets: NID name database, translation files
$distAssets = Join-Path $distDir "assets"
New-Item -ItemType Directory $distAssets -Force | Out-Null
Stage-File (Join-Path $assetsDir "nid_db.txt")   (Join-Path $distAssets "nid_db.txt")
Stage-Dir  (Join-Path $assetsDir "lang")          (Join-Path $distDir "lang")

# Optional external Lua init script (otherwise the embedded one in pcsx5_core.dll is used)
$externalLua = Join-Path $assetsDir "pcsx5_init.lua"
if (Test-Path $externalLua) {
    Stage-File $externalLua (Join-Path $distAssets "pcsx5_init.lua")
} else {
    Log "  (no external pcsx5_init.lua - using embedded script in pcsx5_core.dll)"
}

# Default config
$distConfigDir = Join-Path $distDir "pcsx5_config"
$srcGlobal = Join-Path $repoRoot "pcsx5_config\global.json"
if (Test-Path $srcGlobal) {
    Stage-File $srcGlobal (Join-Path $distConfigDir "global.json")
} else {
    New-Item -ItemType Directory $distConfigDir -Force | Out-Null
    $jsonLines = @(
        '{',
        '    "schema_version": 3,',
        '    "logging": { "min_level": "Info" },',
        '    "crash": { "bundle_dir": "pcsx5_crash" },',
        '    "graphics": { "headless": false, "vsync": true, "vrr": false },',
        '    "audio": { "backend": 2, "volume": 1.0 },',
        '    "input": { "backend": 0 },',
        '    "loader": { "firmware_modules_dir": "" }',
        '}'
    )
    $defaultConfig = $jsonLines -join "`r`n"
    Set-Content -Path (Join-Path $distConfigDir "global.json") -Value $defaultConfig -Encoding UTF8
    Log "  + global.json (default)"
}

# SharpEmu FFmpeg plugins - video decoder runtime DLLs.
# These are probed dynamically by pcsx5_core.dll at runtime;
# place them next to the exe or in a subdirectory.
$sharpemuPluginDirs = @(
    "I:\InstalledGames\sharpemu-win64-fbf2c2d\plugins",
    ".\sharpemu_plugins",
    "..\sharpemu_plugins"
)
$ffmpegDlls = @("avformat-61.dll", "avcodec-61.dll", "avutil-59.dll",
                 "swscale-8.dll", "swresample-5.dll")
$ffmpegFound = $false
foreach ($pDir in $sharpemuPluginDirs) {
    if (Test-Path $pDir) {
        $foundCount = 0
        foreach ($dll in $ffmpegDlls) {
            $src = Join-Path $pDir $dll
            if (Test-Path $src) {
                Stage-File $src (Join-Path $distDir $dll)
                $foundCount++
            }
        }
        if ($foundCount -gt 0) {
            Log "  (FFmpeg: $foundCount DLL(s) staged from $pDir)"
            $ffmpegFound = $true
            break
        }
    }
}
if (-not $ffmpegFound) {
    Log "  (no FFmpeg DLLs found - video decoding unavailable)"
}

# Runtime directories
foreach ($d in @("pcsx5_crash", "pcsx5_savedata", "Cache")) {
    New-Item -ItemType Directory (Join-Path $distDir $d) -Force | Out-Null
}

# Launcher batch file
$batLines = @(
    '@echo off',
    'cd /d "%~dp0"',
    'start "" "pcsx5.exe" %*'
)
Set-Content -Path (Join-Path $distDir "PCSX5.bat") -Value ($batLines -join "`r`n") -Encoding ASCII

# CLI launcher batch
$cliLines = @(
    '@echo off',
    'cd /d "%~dp0"',
    'pcsx5_cli.exe %*'
)
Set-Content -Path (Join-Path $distDir "run.bat") -Value ($cliLines -join "`r`n") -Encoding ASCII

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
