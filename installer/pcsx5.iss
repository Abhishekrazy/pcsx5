; PCSX5 Inno Setup installer script
; Build with: ISCC.exe /DMyAppVersion=1.2.3 installer\pcsx5.iss
; (or via build_and_package.ps1 -CreateInstaller -Version 1.2.3)

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#define MyAppName "PCSX5"
#define MyAppPublisher "PCSX5 Team"
#define MyAppExeName "pcsx5_ui.exe"

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
; Persist the game folder picked on the custom wizard page into config.ini.
; If the user left it blank, GetGameFolder returns the packaged default value.
Filename: "{app}\config.ini"; Section: "Paths"; Key: "GameFolders"; String: "{code:GetGameFolder}"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
var
  GameFolderPage: TInputDirWizardPage;

procedure InitializeWizard;
begin
  { Page with a Browse button letting the user pick where their PS5 games live. }
  GameFolderPage := CreateInputDirPage(wpSelectDir,
    'Games Folder', 'Where are your PS5 game dumps stored?',
    'PCSX5 will scan this folder for installed games. You can add or change ' +
    'folders later in the UI settings. Leave it empty to keep the default, ' +
    'then click Next.',
    False, '');
  GameFolderPage.Add('Games folder (optional):');
  GameFolderPage.Values[0] := '';
end;

function GetGameFolder(Param: String): String;
begin
  Result := Trim(GameFolderPage.Values[0]);
  if Result = '' then
    Result := ExpandConstant('{app}\Games');
end;
