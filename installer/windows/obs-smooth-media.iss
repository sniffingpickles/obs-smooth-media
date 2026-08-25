#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#define AppName "Smooth Media Source"
#define PluginName "obs-smooth-media"
#define ProjectUrl "https://github.com/sniffingpickles/obs-smooth-media"

[Setup]
AppId={{ECA73846-A722-40C3-BCEF-B71DA66269A9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Smooth Media Source contributors
AppPublisherURL={#ProjectUrl}
AppSupportURL={#ProjectUrl}/issues
AppUpdatesURL={#ProjectUrl}/releases
DefaultDirName={code:GetDefaultObsDirectory}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
DirExistsWarning=no
LicenseFile=..\..\LICENSE
OutputDir=..\..\installer-output
OutputBaseFilename={#PluginName}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#AppName}

[Files]
Source: "..\..\artifact\obs-plugins\64bit\{#PluginName}.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion replacesameversion
Source: "..\..\artifact\data\obs-plugins\{#PluginName}\*"; DestDir: "{app}\data\obs-plugins\{#PluginName}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "update.ps1"; DestDir: "{app}\data\obs-plugins\{#PluginName}"; Flags: ignoreversion

[Registry]
Root: HKLM; Subkey: "Software\Smooth Media Source"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Smooth Media Source"; ValueType: string; ValueName: "ObsPath"; ValueData: "{app}"

[Icons]
Name: "{group}\Check for updates"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\data\obs-plugins\{#PluginName}\update.ps1"""; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Code]
function IsObsDirectory(const Directory: String): Boolean;
begin
  Result := FileExists(AddBackslash(Directory) + 'bin\64bit\obs64.exe');
end;

function GetDefaultObsDirectory(Param: String): String;
var
  Directory: String;
begin
  if RegQueryStringValue(HKLM64, 'Software\OBS Studio', '', Directory) and
     IsObsDirectory(Directory) then
  begin
    Result := Directory;
    Exit;
  end;

  if RegQueryStringValue(HKLM32, 'Software\OBS Studio', '', Directory) and
     IsObsDirectory(Directory) then
  begin
    Result := Directory;
    Exit;
  end;

  Result := ExpandConstant('{autopf}\obs-studio');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not IsObsDirectory(WizardDirValue) then
  begin
    MsgBox('Choose the OBS Studio folder that contains bin\64bit\obs64.exe.',
      mbError, MB_OK);
    Result := False;
  end;
end;
