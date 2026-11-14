; Aligna Windows installer (Inno Setup 6). Built by .github/workflows/build.yml:
;   ISCC /DAppVersion=0.3.0 /DSourceExe=<path>\Aligna.exe /DBuildDir=<build dir> /DRepoDir=<repo root> /O<output dir> Aligna.iss
; Installs for the current user only (no administrator prompt) and registers .aligna projects.

#ifndef AppVersion
  #error AppVersion is not defined
#endif
#ifndef SourceExe
  #error SourceExe is not defined
#endif
#ifndef BuildDir
  #error BuildDir is not defined
#endif
#ifndef RepoDir
  #error RepoDir is not defined
#endif

[Setup]
; Never change AppId: it is how a newer installer finds and replaces an older install.
AppId={{6B0E2C5E-7B5F-4D0B-9A51-3C1D7E2F4A90}
AppName=Aligna
AppVersion={#AppVersion}
AppVerName=Aligna {#AppVersion}
AppPublisher=Aligna
AppPublisherURL=https://github.com/erikhector/Aligna
AppSupportURL=https://github.com/erikhector/Aligna/issues
AppUpdatesURL=https://github.com/erikhector/Aligna/releases
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\Aligna
DisableProgramGroupPage=yes
DisableDirPage=auto
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
; An update started from Aligna runs while Aligna is still closing.
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=Aligna
UninstallDisplayIcon={app}\Aligna.exe
OutputBaseFilename=Aligna-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\LICENSE-LAME.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\LICENSE-RubberBand.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Aligna"; Filename: "{app}\Aligna.exe"
Name: "{autodesktop}\Aligna"; Filename: "{app}\Aligna.exe"; Tasks: desktopicon

[Registry]
; Double-clicking a .aligna project opens it in Aligna.
Root: HKA; Subkey: "Software\Classes\.aligna"; ValueType: string; ValueName: ""; ValueData: "Aligna.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.aligna\OpenWithProgids"; ValueType: string; ValueName: "Aligna.Project"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Aligna.Project"; ValueType: string; ValueName: ""; ValueData: "Aligna Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Aligna.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Aligna.exe,0"
Root: HKA; Subkey: "Software\Classes\Aligna.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Aligna.exe"" ""%1"""

[Run]
; Installed by hand: a "Launch Aligna" box on the last page.
Filename: "{app}\Aligna.exe"; Description: "{cm:LaunchProgram,Aligna}"; Flags: nowait postinstall skipifsilent
; Installed as an update from inside Aligna (/SILENT /relaunch=1): open Aligna again.
Filename: "{app}\Aligna.exe"; Flags: nowait; Check: RelaunchRequested

[Code]
function RelaunchRequested: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:relaunch|0}') = '1');
end;
