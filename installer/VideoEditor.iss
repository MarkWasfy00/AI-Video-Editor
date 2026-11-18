; Inno Setup script for the Windows installer.
; Build dist/VideoEditor first (scripts/deploy-windows.sh), then:
;   ISCC.exe installer\VideoEditor.iss
; Output: dist\VideoEditor-Setup-<version>.exe

#define AppName "AI Video Editor"
#define AppVersion "0.2.0"
#define AppExe "VideoEditor.exe"

[Setup]
AppId={{6F3B2C1E-8A4D-4E7B-9C2F-1D5A7E9B3C40}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Mark Wasfy
AppPublisherURL=https://github.com/MarkWasfy00/AI-Video-Editor
DefaultDirName={autopf}\AI Video Editor
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=VideoEditor-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExe}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\VideoEditor\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "__pycache__\*,*.pyc"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
