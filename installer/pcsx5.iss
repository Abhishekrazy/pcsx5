; PCSX5 Inno Setup installer script
; Build with: ISCC.exe /DMyAppVersion=1.2.3 installer\pcsx5.iss
; (or via build_and_package.ps1 -CreateInstaller -Version 1.2.3)

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#define MyAppName "PCSX5"
#define MyAppPublisher "PCSX5 Team"
#define MyAppExeName "pcsx5.exe"

[Setup]
AppId={{7F3A2B91-4C5E-4D6A-9B8F-1E2C3D4A5B6C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Info (introduction) page shown before the license page
InfoBeforeFile=INTRO.txt
; Terms & Conditions page
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=PCSX5-{#MyAppVersion}-win64-Setup
SetupIconFile=..\assets\PCSX5_Logo.ico
UninstallDisplayIcon={app}\PCSX5_Logo.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[INI]
; The games folder is picked on first launch inside the UI, so the installer
; leaves it blank; the app detects the empty value and shows its first-run
; setup dialog.
Filename: "{app}\config.ini"; Section: "Paths"; Key: "GameFolders"; String: ""

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
