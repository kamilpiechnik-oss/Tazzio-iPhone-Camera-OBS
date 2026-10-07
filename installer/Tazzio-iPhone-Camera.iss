#define AppName "Tazzio iPhone Camera"
#define AppVersion "1.0.1"
#ifndef BuildRoot
  #define BuildRoot "..\obs-plugin\release\tazzio-iphone-camera"
#endif
#ifndef InstallerOutput
  #define InstallerOutput "."
#endif
[Setup]
AppId={{DC28D2A1-C5E8-4AC1-91BB-B65FB094D217}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\obs-studio
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
OutputBaseFilename=Tazzio-iPhone-Camera-Setup-1.0.1
OutputDir={#InstallerOutput}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
[Files]
Source: "{#BuildRoot}\bin\64bit\tazzio-iphone-camera.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#BuildRoot}\runtime\tls\qschannelbackend.dll"; DestDir: "{app}\bin\64bit\tls"; Flags: ignoreversion
Source: "{#BuildRoot}\data\*"; DestDir: "{app}\data\obs-plugins\tazzio-iphone-camera"; Flags: ignoreversion recursesubdirs createallsubdirs
[Run]
Filename: "{app}\bin\64bit\obs64.exe"; Description: "Uruchom OBS Studio"; Flags: nowait postinstall skipifsilent
[Code]
function InitializeSetup(): Boolean;
begin
  Result := DirExists(ExpandConstant('{autopf}\obs-studio'));
  if not Result then MsgBox('Nie znaleziono OBS Studio w standardowej lokalizacji.', mbError, MB_OK);
end;
