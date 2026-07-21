<#
.SYNOPSIS
    Builds PCSX5 in Release configuration and creates a distribution package.

.DESCRIPTION
    This script builds the PCSX5 emulator using CMake/Ninja in Release configuration,
    then packages all necessary files into a distributable folder structure.

.EXAMPLE
    .\build_and_package.ps1
    .\build_and_package.ps1 -CreateZip
    .\build_and_package.ps1 -OutputDir "C:\Dist\PCSX5"
#>

param(
    [Parameter(Mandatory=$false, Position=0)]
    [string]$OutputDir = "dist",

    [Parameter(Mandatory=$false)]
    [switch]$CreateZip,

    [Parameter(Mandatory=$false)]
    [string]$BuildConfig = "Release",

    [Parameter(Mandatory=$false)]
    [switch]$CleanBuild,

    [Parameter(Mandatory=$false)]
    [switch]$SkipBuild,

    [Parameter(Mandatory=$false)]
    [switch]$CreateInstaller,

    [Parameter(Mandatory=$false)]
    [string]$Version = "0.0.0-dev"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $scriptDir "build"
$distDir = Join-Path $scriptDir $OutputDir
$assetsDir = Join-Path $scriptDir "assets"

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host "[$timestamp] [$Level] $Message"
}

function Write-ErrorAndExit {
    param([string]$Message)
    Write-Log $Message "ERROR"
    exit 1
}

# Clean previous build if requested
if ($CleanBuild -and (Test-Path $buildDir)) {
    Write-Log "Cleaning previous build directory..."
    Remove-Item -Recurse -Force $buildDir
}

# Build the project
if (-not $SkipBuild) {
    Write-Log "Configuring CMake project..."
    if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
        cmake -B $buildDir -G "Ninja" -DCMAKE_BUILD_TYPE=$BuildConfig $scriptDir
        if ($LASTEXITCODE -ne 0) { Write-ErrorAndExit "CMake configuration failed" }
    }

    Write-Log "Building PCSX5 ($BuildConfig)..."
    cmake --build $buildDir --config $BuildConfig
    if ($LASTEXITCODE -ne 0) { Write-ErrorAndExit "Build failed" }
    Write-Log "Build completed successfully"
} else {
    Write-Log "Skipping build (--SkipBuild specified)"
}

# Verify build outputs exist
$basePath = Join-Path $buildDir $BuildConfig

# Ninja builds place pcsx5.exe under build\bin (RUNTIME_OUTPUT_DIRECTORY in
# CMakeLists.txt), while VS-generator builds use build\<Config>.  Prefer the
# freshest copy so -SkipBuild never packages a stale exe.
$pcsx5Exe = Join-Path $basePath "pcsx5.exe"
$pcsx5ExeBin = Join-Path (Join-Path $buildDir "bin") "pcsx5.exe"
if ((Test-Path $pcsx5ExeBin) -and
    (-not (Test-Path $pcsx5Exe) -or
     (Get-Item $pcsx5ExeBin).LastWriteTime -gt (Get-Item $pcsx5Exe).LastWriteTime)) {
    $pcsx5Exe = $pcsx5ExeBin
}

# Create required files array using individual variables to avoid Join-Path array parsing issue
$f1 = $pcsx5Exe
$f2 = Join-Path $basePath "pcsx5_ui.exe"
$f3 = Join-Path $basePath "pcsx5_ui.dll"
$f4 = Join-Path $basePath "pcsx5_ui.deps.json"
$f5 = Join-Path $basePath "pcsx5_ui.runtimeconfig.json"
$f6 = Join-Path $basePath "config.ini"
$f7 = Join-Path $basePath "pcsx5_ui.pdb"
$requiredFiles = @($f1, $f2, $f3, $f4, $f5, $f6, $f7)

$missingFiles = @()
foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file)) {
        $missingFiles += $file
    }
}

if ($missingFiles.Count -gt 0) {
    Write-Log "Missing build outputs:" "WARN"
    $missingFiles | ForEach-Object { Write-Log "  $_" "WARN" }
    Write-ErrorAndExit "Some build outputs are missing. Build may have failed."
}

# Clean and create distribution directory
if (Test-Path $distDir) {
    Write-Log "Cleaning existing distribution directory..."
    Remove-Item -Recurse -Force $distDir
}

Write-Log "Creating distribution directory: $distDir"
New-Item -ItemType Directory -Path $distDir -Force | Out-Null

# Copy main executables and DLLs
Write-Log "Copying executables and DLLs..."
$filesToCopy = @(
    @{ Source = $pcsx5Exe; Dest = Join-Path $distDir "pcsx5.exe" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_ui.exe"; Dest = Join-Path $distDir "pcsx5_ui.exe" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_ui.dll"; Dest = Join-Path $distDir "pcsx5_ui.dll" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_ui.deps.json"; Dest = Join-Path $distDir "pcsx5_ui.deps.json" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_ui.runtimeconfig.json"; Dest = Join-Path $distDir "pcsx5_ui.runtimeconfig.json" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "config.ini"; Dest = Join-Path $distDir "config.ini" },
    @{ Source = Join-Path $buildDir "pcsx5_snd_decode.exe"; Dest = Join-Path $distDir "pcsx5_snd_decode.exe" },
    @{ Source = Join-Path $buildDir "dualsense_visual.exe"; Dest = Join-Path $distDir "dualsense_visual.exe" }
)

foreach ($item in $filesToCopy) {
    if (Test-Path $item.Source) {
        Copy-Item -Path $item.Source -Destination $item.Dest -Force
        Write-Log "  Copied: $(Split-Path $item.Dest -Leaf)"
    } else {
        Write-Log "  WARNING: Source not found: $($item.Source)" "WARN"
    }
}

# Copy directories
Write-Log "Copying directories..."
$dirsToCopy = @(
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_config"; Dest = Join-Path $distDir "pcsx5_config" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "pcsx5_crash"; Dest = Join-Path $distDir "pcsx5_crash" },
    @{ Source = Join-Path (Join-Path $buildDir $BuildConfig) "Cache"; Dest = Join-Path $distDir "Cache" }
)

foreach ($dir in $dirsToCopy) {
    if (Test-Path $dir.Source) {
        Copy-Item -Path $dir.Source -Destination $dir.Dest -Recurse -Force
        Write-Log "  Copied directory: $(Split-Path $dir.Dest -Leaf)"
    } else {
        Write-Log "  WARNING: Source directory not found: $($dir.Source)" "WARN"
        # Create empty directory anyway
        New-Item -ItemType Directory -Path $dir.Dest -Force | Out-Null
    }
}

# Copy language assets
Write-Log "Copying language assets..."
$langSource = Join-Path $assetsDir "lang"
$langDest = Join-Path $distDir "lang"
if (Test-Path $langSource) {
    Copy-Item -Path $langSource -Destination $langDest -Recurse -Force
    $langCount = (Get-ChildItem -Path $langDest -Filter "*.json").Count
    Write-Log "  Copied $langCount language files"
} else {
    Write-Log "  WARNING: Language directory not found: $langSource" "WARN"
}

# Copy logo assets
Write-Log "Copying logo assets..."
$logoFiles = @("PCSX5_Logo.ico", "PCSX5_Logo.png")
foreach ($logo in $logoFiles) {
    $src = Join-Path $assetsDir $logo
    $dst = Join-Path $distDir $logo
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $dst -Force
        Write-Log "  Copied: $logo"
    } else {
        Write-Log "  WARNING: Logo not found: $logo" "WARN"
    }
}

# Create a simple launcher batch file for convenience
Write-Log "Creating launcher scripts..."
$launcherContent = @"
@echo off
REM PCSX5 Launcher
REM Launches the PCSX5 UI which manages the emulator process

cd /d "%~dp0"
start "" "pcsx5_ui.exe" %*
"@
Set-Content -Path (Join-Path $distDir "PCSX5.bat") -Value $launcherContent -Encoding ASCII
Write-Log "  Created: PCSX5.bat"

# Create a README for the distribution
$readmeContent = @"
# PCSX5 Distribution Package

This package contains the PCSX5 PlayStation 5 emulator and its UI frontend.

## Contents

- `pcsx5.exe` - Main emulator core
- `pcsx5_ui.exe` - WPF UI frontend (Windows 11 Mica/Fluent Design)
- `pcsx5_ui.dll` - UI library
- `pcsx5_ui.deps.json` / `pcsx5_ui.runtimeconfig.json` - .NET 9 runtime configuration
- `pcsx5_snd_decode.exe` - Standalone ATRAC9/OGG to WAV sound decoder
- `config.ini` - Default configuration file
- `pcsx5_config/` - Configuration directory
- `pcsx5_crash/` - Crash dump directory
- `Cache/` - Shader/cache directory
- `lang/` - Language files (11 languages)
- `PCSX5_Logo.ico` / `PCSX5_Logo.png` - Application icons

## Requirements

- Windows 10/11 (64-bit)
- .NET 9.0 Desktop Runtime
- Vulkan-compatible GPU
- Visual C++ Redistributable (latest)

## Usage

Run `PCSX5.bat` or `pcsx5_ui.exe` to launch the emulator UI.
The UI will manage the emulator process automatically.

For command-line usage:
```
pcsx5.exe --help
```

## Sound Decoder

Use `pcsx5_snd_decode.exe` to decode ATRAC9/OGG audio files:
```
pcsx5_snd_decode.exe <input.at9> <output.wav>
```

## Configuration

Edit `config.ini` to customize paths, graphics, audio, and input settings.
Language can be changed in the UI or by editing the `[Ui] Language` setting in config.ini.

Supported languages: en-US, de-DE, es-ES, fr-FR, it-IT, ja-JP, ko-KR, pt-BR, ru-RU, zh-CN, zh-TW
"@
Set-Content -Path (Join-Path $distDir "README_DISTRIBUTION.txt") -Value $readmeContent -Encoding UTF8
Write-Log "  Created: README_DISTRIBUTION.txt"

# Create ZIP archive if requested
if ($CreateZip) {
    $zipName = "PCSX5_$(Get-Date -Format 'yyyyMMdd')_$BuildConfig.zip"
    $zipPath = Join-Path $scriptDir $zipName
    
    Write-Log "Creating ZIP archive: $zipName..."
    Compress-Archive -Path (Join-Path $distDir "*") -DestinationPath $zipPath -Force
    Write-Log "  Created: $zipPath"
    Write-Log "  Size: $([math]::Round((Get-Item $zipPath).Length / 1MB, 2)) MB"
}

# Build the Inno Setup installer if requested
if ($CreateInstaller) {
    Write-Log "Locating Inno Setup compiler (ISCC.exe)..."
    $iscc = $null

    if ($env:INNO_SETUP -and (Test-Path $env:INNO_SETUP)) {
        $iscc = $env:INNO_SETUP
    }

    if (-not $iscc) {
        $uninstallRoots = @(
            "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*",
            "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*",
            "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*"
        )
        foreach ($root in $uninstallRoots) {
            $entry = Get-ItemProperty -Path $root -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($entry -and $entry.InstallLocation) {
                $candidate = Join-Path $entry.InstallLocation "ISCC.exe"
                if (Test-Path $candidate) { $iscc = $candidate; break }
            }
        }
    }

    if (-not $iscc) {
        $fallback = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
        if (Test-Path $fallback) { $iscc = $fallback }
    }

    if (-not $iscc) {
        Write-ErrorAndExit "Inno Setup 6 (ISCC.exe) not found. Install it from https://jrsoftware.org/isinfo.php or set the INNO_SETUP environment variable to ISCC.exe."
    }

    Write-Log "Compiling installer with: $iscc (version $Version)"
    $issPath = Join-Path $scriptDir "installer\pcsx5.iss"
    & $iscc "/DMyAppVersion=$Version" $issPath
    if ($LASTEXITCODE -ne 0) { Write-ErrorAndExit "Inno Setup compilation failed (exit code $LASTEXITCODE)" }

    $setupExe = Get-ChildItem -Path (Join-Path $scriptDir "installer\Output") -Filter "*-Setup.exe" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($setupExe) {
        Write-Log "  Created: $($setupExe.FullName)"
        Write-Log "  Size: $([math]::Round($setupExe.Length / 1MB, 2)) MB"
    }
}

# Summary
Write-Log "========================================="
Write-Log "DISTRIBUTION PACKAGE CREATED SUCCESSFULLY"
Write-Log "========================================="
Write-Log "Output directory: $distDir"
Write-Log "Contents:"
Get-ChildItem -Path $distDir -Recurse | ForEach-Object {
    $size = if (-not $_.PSIsContainer) { " ($([math]::Round($_.Length / 1KB, 1)) KB)" } else { "" }
    Write-Log "  $($_.FullName.Substring($distDir.Length + 1))$size"
}

if ($CreateZip) {
    Write-Log "ZIP archive: $zipPath"
}