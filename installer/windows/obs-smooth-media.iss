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
Source: "..\..\artifact\variants\ffmpeg61\{#PluginName}.dll"; DestDir: "{app}\obs-plugins\64bit"; DestName: "{#PluginName}.dll"; Flags: ignoreversion; Check: UseFfmpeg61
Source: "..\..\artifact\variants\ffmpeg62\{#PluginName}.dll"; DestDir: "{app}\obs-plugins\64bit"; DestName: "{#PluginName}.dll"; Flags: ignoreversion; Check: UseFfmpeg62
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

function GetFfmpegAbi(const Directory: String): Integer;
var
  BinDirectory: String;
begin
  BinDirectory := AddBackslash(Directory) + 'bin\64bit\';
  if FileExists(BinDirectory + 'avformat-62.dll') then
    Result := 62
  else if FileExists(BinDirectory + 'avformat-61.dll') then
    Result := 61
  else
    Result := 0;
end;

function UseFfmpeg61: Boolean;
begin
  Result := GetFfmpegAbi(ExpandConstant('{app}')) = 61;
end;

function UseFfmpeg62: Boolean;
begin
  Result := GetFfmpegAbi(ExpandConstant('{app}')) = 62;
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not IsObsDirectory(ExpandConstant('{app}')) then
    Result := 'OBS Studio was not found in the selected folder.'
  else if GetFfmpegAbi(ExpandConstant('{app}')) = 0 then
    Result := 'This OBS version is not supported. Smooth Media Source currently supports 64-bit OBS 32.0 through 32.2.';
end;
