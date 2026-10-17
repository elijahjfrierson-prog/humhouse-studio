; Produces HumHouse-Studio-Windows-Setup.exe: shows the EULA (must be accepted), then installs
; HumHouse Studio into Program Files with Start-menu and optional desktop shortcuts.
#define MyAppName "HumHouse Studio"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "HumHouse"
#define MyAppURL "https://github.com/elijahjfrierson-prog/humhouse-studio"
#define MyAppExeName "HumHouse Studio.exe"

[Setup]
AppId={{B3E1C7A2-5F44-4C1D-9E2B-HUMHOUSESTUDIO}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\HumHouse\HumHouse Studio
DefaultGroupName=HumHouse
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE.txt
OutputDir=..\..\
OutputBaseFilename=HumHouse-Studio-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}

[InstallDelete]
Type: files; Name: "{app}\{#MyAppExeName}"

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\..\build\HumHouseStudio_artefacts\Release\HumHouse Studio.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\HumHouse Studio"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall HumHouse Studio"; Filename: "{uninstallexe}"
Name: "{commondesktop}\HumHouse Studio"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch HumHouse Studio"; Flags: nowait postinstall skipifsilent
