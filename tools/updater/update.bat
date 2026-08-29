@echo off
echo Starting PCSX5 Updater...
python tools\updater\pcsx5_update.py
if %ERRORLEVEL% neq 0 (
    echo Update failed.
    pause
    exit /b %ERRORLEVEL%
)
echo Launching emulator...
start "" build\bin\Release\pcsx5_cli.exe
