; Inno Setup script for the Windows installer. CI passes Version, Stage (the
; folder with obs-plugins\ and data\) and OutDir on the command line.
; Installs the same layout as the ZIP into the chosen OBS folder.
#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppId={{6B1F3C52-6F0E-4C55-9C2B-3E7C7A0B5D21}
AppName=Plugin Checker for OBS Studio
AppVersion={#Version}
AppPublisher=n3ckarmann
AppPublisherURL=https://github.com/n3ckarmann/OBSPluginChecker
AppSupportURL=https://github.com/n3ckarmann/OBSPluginChecker/issues
DefaultDirName={code:DefaultObsDir}
DirExistsWarning=no
AppendDefaultDirName=no
DisableProgramGroupPage=yes
UninstallDisplayName=Plugin Checker for OBS Studio
OutputDir={#OutDir}
OutputBaseFilename=obs-plugin-checker-{#Version}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=..\..\LICENSE.md
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
en.SelectObs=Select the OBS Studio folder (the one containing bin, data and obs-plugins). For a portable OBS, choose its folder.
de.SelectObs=Wähle den OBS-Studio-Ordner (den mit bin, data und obs-plugins). Bei einer portablen OBS wähle deren Ordner.
en.NoObs=No OBS Studio found in this folder (bin\64bit\obs64.exe is missing). Please choose the OBS folder.
de.NoObs=In diesem Ordner wurde kein OBS Studio gefunden (bin\64bit\obs64.exe fehlt). Bitte wähle den OBS-Ordner.
en.ObsRunning=Please quit OBS Studio before installing.
de.ObsRunning=Bitte beende OBS Studio vor der Installation.

[Files]
Source: "{#Stage}\obs-plugins\64bit\*"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#Stage}\data\obs-plugins\obs-plugin-checker\*"; DestDir: "{app}\data\obs-plugins\obs-plugin-checker"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function DefaultObsDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

procedure InitializeWizard;
begin
  WizardForm.SelectDirLabel.Caption := CustomMessage('SelectObs');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and
     not FileExists(AddBackslash(WizardDirValue) + 'bin\64bit\obs64.exe') then
  begin
    SuppressibleMsgBox(CustomMessage('NoObs'), mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  { find exits with 0 only when tasklist lists a running obs64.exe }
  if Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul',
          '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
    Result := CustomMessage('ObsRunning');
end;
