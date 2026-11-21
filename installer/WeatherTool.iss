; WeatherTool installer (Inno Setup 6), modelled on BriskMail's. Built by tools/release.ps1,
; which passes the version (checked against the exe's version resource) and the paths;
; compiling this file by hand without them stops.
;
; Machine-level only, into Program Files: no per-user choice. A copy under Program Files keeps
; WeatherTool.ini and its cache\ folder in %ProgramData%\WeatherTool (src/config.cpp,
; ResolveDataDir). This installer creates that folder with Users modify, inherited by what the
; app creates inside it, so a second person on a shared PC can save too; without it the first
; one to run the app owns the folder.
;
; Uninstall leaves WeatherTool.ini (the places and settings, for a reinstall) and deletes
; cache\, which the app downloads again.
;
; Help > Check for updates in an installed copy downloads the next release's setup and runs it
; with /SILENT /SUPPRESSMSGBOXES /NORESTART /ALLUSERS /WEATHERTOOLRESTART=1 once WeatherTool has
; closed (update::SetupArgs); the second [Run] entry starts WeatherTool again only then, never
; after an ordinary silent install (an admin deploying it).
;
; The setup carries exactly what the portable zip does: WeatherTool.exe and README.md.

#ifndef AppVersion
  #error AppVersion must be passed with /DAppVersion
#endif
#ifndef AppExe
  #error AppExe (the built WeatherTool.exe) must be passed with /DAppExe
#endif
#ifndef SourceDir
  #error SourceDir must be passed with /DSourceDir
#endif
#ifndef OutDir
  #error OutDir must be passed with /DOutDir
#endif

#define AppName "WeatherTool"
; The data folder under %ProgramData%; ResolveDataDir names the same one.
#define DataFolder "WeatherTool"

[Setup]
AppId={{BD7E143C-087E-4D1E-A968-49C966C6FD50}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=B Graham
AppPublisherURL=https://github.com/Flinterpop/WeatherTool
AppSupportURL=https://github.com/Flinterpop/WeatherTool/issues
AppUpdatesURL=https://github.com/Flinterpop/WeatherTool/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=WeatherTool-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#SourceDir}\src\WeatherTool.ico
UninstallDisplayIcon={app}\WeatherTool.exe
UninstallDisplayName={#AppName} {#AppVersion}
; WeatherTool may be running; Setup asks to close it rather than replace it in use.
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; DestName: "WeatherTool.exe"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; Settings and the download cache. Created here so the first person to run the app does not own
; it and lock the next one out. The folder itself is never removed by uninstall.
Name: "{commonappdata}\{#DataFolder}"; Permissions: users-modify; Flags: uninsneveruninstall

[UninstallDelete]
; Regenerable: the app downloads it again. WeatherTool.ini stays.
Type: filesandordirs; Name: "{commonappdata}\{#DataFolder}\cache"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\WeatherTool.exe"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\WeatherTool.exe"; Tasks: desktopicon

[Run]
; An interactive install offers to start it; WeatherTool updating itself starts it again.
; runasoriginaluser: start it as the person who ran Setup, not the elevated account.
Filename: "{app}\WeatherTool.exe"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "{app}\WeatherTool.exe"; Flags: nowait runasoriginaluser; Check: SelfUpdate

[Code]
// Setup was run by WeatherTool's own Check for updates (update::SetupArgs passes this).
function SelfUpdate: Boolean;
begin
  Result := ExpandConstant('{param:WEATHERTOOLRESTART|0}') = '1';
end;
