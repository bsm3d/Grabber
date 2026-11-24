; Grabber
; © Benoit Saint-Moulin - 2026
; Version du fichier : 1.2.0
; Fonction du fichier : Inno Setup script that packs the Grabber distribution folder into a per-user installer with version and icons.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define AppName "Grabber"
#define AppExe "Grabber.exe"
; folder of the finished installers, the same for every app (BSM_DISTRIBUTIONS overrides the default)
#define DistRoot GetEnv("BSM_DISTRIBUTIONS")
#if DistRoot == ""
  #define DistRoot "D:\Documents\Coding\distributions"
#endif

[Setup]
AppId={{3F6C2A9E-5B1D-4E7A-8C34-1D9A7B50E2C6}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Benoit Saint-Moulin
AppPublisherURL=https://www.bsm3d.com
AppSupportURL=https://github.com/bsm3d/Grabber/
AppUpdatesURL=https://github.com/bsm3d/Grabber/releases
; per user, no administrator rights needed
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\Grabber
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=..\capture.ico
WizardImageFile=..\wizard.bmp
WizardSmallImageFile=..\wizard_small.bmp
OutputDir={#DistRoot}\Grabber
OutputBaseFilename=Grabber-{#AppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=..\LICENSE.txt

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; what the older (PyInstaller) version left beside the program
Type: filesandordirs; Name: "{app}\_internal"
Type: files; Name: "{app}\base_library.zip"

[Files]
Source: "..\dist\Grabber\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; ---- user data question (added by the audit, group 2) ----
[CustomMessages]
english.DataAsk=Remove all the user data of %1?%n%nFolder(s):%n%2%nContent: %3 (%4 MB).%n%nYes: the folder(s) are deleted, for good.%nNo: they are kept, you get them back if you reinstall %1.%n%nYour data in D:\Datas is never touched: delete it yourself if you do not want it.
english.DataWhat=the settings and presets (your videos are never touched)
french.DataAsk=Retirer toutes les données utilisateur de %1 ?%n%nDossier(s) :%n%2%nContenu : %3 (%4 Mo).%n%nOui : le ou les dossiers sont supprimés, définitivement.%nNon : ils sont conservés, tu les retrouves si tu réinstalles %1.%n%nTes données de D:\Datas ne sont jamais touchées : supprime-les toi-même si tu n'en veux plus.
french.DataWhat=les réglages et préréglages (tes vidéos ne sont jamais touchées)

[Code]
// Size of a folder in bytes: the dialog tells the user what "Yes" removes.
function FolderBytes(const Dir: String): Int64;
var
  Rec: TFindRec;
begin
  Result := 0;
  if FindFirst(Dir + '\*', Rec) then
  try
    repeat
      if (Rec.Name <> '.') and (Rec.Name <> '..') then
      begin
        if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          Result := Result + FolderBytes(Dir + '\' + Rec.Name)
        else
          Result := Result + (Int64(Rec.SizeHigh) * 4294967296) + Rec.SizeLow;
      end;
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
end;

procedure AddIfExists(var Dirs: TArrayOfString; const Dir: String);
var
  N: Integer;
begin
  if DirExists(Dir) then
  begin
    N := GetArrayLength(Dirs);
    SetArrayLength(Dirs, N + 1);
    Dirs[N] := Dir;
  end;
end;

// One question, Yes or No, No by default: remove all the user data? The program and its technical leftovers are always removed.
// A silent uninstall (/SILENT, /VERYSILENT) asks nothing and keeps the data: /SUPPRESSMSGBOXES does not answer a MsgBox of
// this script, so the uninstaller would wait for a click nobody can make.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dirs: TArrayOfString;
  Where: String;
  Bytes: Int64;
  I: Integer;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  if UninstallSilent then Exit;
  SetArrayLength(Dirs, 0);
  AddIfExists(Dirs, ExpandConstant('{userappdata}\BSM\Grabber'));
  if GetArrayLength(Dirs) = 0 then Exit;
  Where := '';
  Bytes := 0;
  for I := 0 to GetArrayLength(Dirs) - 1 do
  begin
    if Where <> '' then Where := Where + #13#10;
    Where := Where + Dirs[I];
    Bytes := Bytes + FolderBytes(Dirs[I]);
  end;
  if MsgBox(FmtMessage(CustomMessage('DataAsk'), ['Grabber', Where, CustomMessage('DataWhat'), IntToStr(Integer(Bytes div 1048576))]),
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
  begin
    for I := 0 to GetArrayLength(Dirs) - 1 do
      DelTree(Dirs[I], True, True, True);
    RemoveDir(ExpandConstant('{userappdata}\BSM'));       // only when empty
  end;
end;
