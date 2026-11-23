; CINEMAX installer (Inno Setup 6). Built by build.ps1 -Installer:
;   ISCC /DAppVersion=1.0.0 /DSourceDir=<dist\CINEMAX> /O<dist> installer\cinemax.iss
; Per-user install by default (no admin needed; CINEMAX itself never needs admin).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\CINEMAX"
#endif

[Setup]
AppId={{6E0C2D1B-4B7A-4C55-9C4E-0B8E4B1C7A21}
AppName=CINEMAX
AppVersion={#AppVersion}
AppPublisher=CINEMAX
AppComments=Real-time cinematic audio enhancement for Windows
DefaultDirName={autopf}\CINEMAX
DefaultGroupName=CINEMAX
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=CINEMAX-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
UninstallDisplayIcon={app}\CINEMAX.exe
CloseApplications=yes
RestartApplications=no
InfoAfterFile=postinstall.txt

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "startup"; Description: "Start CINEMAX in the &tray when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\CINEMAX.exe";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\CinemaxGuard.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\cinemax-cli.exe";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\README.md";          DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\TROUBLESHOOTING.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\RECOVERY.md";        DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\VISION-ENGINE.md";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\LICENSE-NOTICE.md";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\onnxruntime.dll";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\onnxruntime_providers_shared.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\models\*";          DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#SourceDir}\hrtf\*";            DestDir: "{app}\hrtf"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\CINEMAX";                 Filename: "{app}\CINEMAX.exe"
Name: "{group}\CINEMAX - Restore Audio"; Filename: "{app}\CinemaxGuard.exe"; Parameters: "--restore"; Comment: "Hand the Windows audio output back to your speakers/headphones"
Name: "{group}\Troubleshooting";         Filename: "{app}\TROUBLESHOOTING.md"
Name: "{autodesktop}\CINEMAX";           Filename: "{app}\CINEMAX.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CINEMAX"; ValueData: """{app}\CINEMAX.exe"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\CINEMAX.exe"; Description: "Launch CINEMAX"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Close CINEMAX if running and hand the Windows default output back before removing files.
Filename: "{app}\CinemaxGuard.exe"; Parameters: "--restore --quiet"; Flags: runhidden waituntilterminated; RunOnceId: "CinemaxRestoreAudio"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\CINEMAX\WebView2"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'CINEMAX');
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'CINEMAX Audio Recovery');
  end;
end;
