; Produces HumHouse-Studio-Windows-Setup.exe: shows the EULA (must be accepted), then installs
; HumHouse Studio into Program Files with Start-menu and optional desktop shortcuts.
#ifdef Master
  #define MyAppName "HumHouse Studio Master"
  #define MyTarget "HumHouseStudioMaster"
  #define MyOut "HumHouse-Studio-Master-Windows-Setup"
#else
  #define MyAppName "HumHouse Studio"
  #define MyTarget "HumHouseStudio"
  #define MyOut "HumHouse-Studio-Windows-Setup"
#endif
#define MyAppVersion "1.3.1"
#define MyAppPublisher "HumHouse"
#define MyAppURL "https://github.com/elijahjfrierson-prog/humhouse-studio"
#define MyAppExeName MyAppName + ".exe"

[Setup]
#ifdef Master
AppId={{B3E1C7A2-5F44-4C1D-9E2B-HUMHOUSEMASTER}}
#else
AppId={{B3E1C7A2-5F44-4C1D-9E2B-HUMHOUSESTUDIO}}
#endif
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\HumHouse\{#MyAppName}
DefaultGroupName=HumHouse
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE.txt
OutputDir=..\..\
OutputBaseFilename={#MyOut}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\..\assets\humhouse.ico
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
CloseApplications=force
RestartApplications=no
UsePreviousAppDir=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}

[InstallDelete]
Type: files; Name: "{app}\{#MyAppExeName}"

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\..\build\{#MyTarget}_artefacts\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\Sounds\*"; DestDir: "{app}\Sounds"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
