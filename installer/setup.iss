; =====================================================================
; 人人 - Inno Setup 6 Installation Script
; Stage 5 Production-Grade Machine Installer
; Target Architecture: Windows 10/11 x64 ONLY
; =====================================================================

#ifndef MyAppVersion
#define MyAppVersion "1.1.0"
#endif

; VersionInfoVersion must be four numeric fields (0-65535 each), so release
; strings like "1.0.0-ci.20261107.0fd389b" cannot be used directly. The build
; script passes a normalized numeric variant through this define.
#ifndef MyAppVersionInfoVersion
#define MyAppVersionInfoVersion "1.1.0.0"
#endif

#define MyAppName "人人输入法"
#define MyAppPublisher "maifeipin"
#define MyAppURL "https://github.com/maifeipin/PersonalIME"
#define MyAppExeName "PersonalIME.Manager.exe"
#define MyServiceExeName "ime-service.exe"

[Setup]
AppId={{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion} (x64)
UninstallDisplayName={#MyAppName}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\PersonalIME
DefaultGroupName={#MyAppName}
LicenseFile=..\LICENSE
OutputDir=..\out\installer
OutputBaseFilename=PersonalIME-Setup-x64
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Executable Metadata & Version Information:
VersionInfoVersion={#MyAppVersionInfoVersion}
VersionInfoTextVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoCopyright=Copyright (C) 2026 {#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersionInfoVersion}

; Clean Machine-Level Privileges Model:
; Admin privileges used strictly for Program Files & Machine COM Registration.
; User data (%APPDATA%) and user profile registry are untouched by admin installer.
PrivilegesRequired=admin

; Safety & Clean In-Use Handling:
; Install PersonalIME.dll into versioned subdirectories so upgrades never
; conflict with host apps holding the previous DLL. Exclude *.dll from
; CloseApplicationsFilter so host apps (Explorer, browsers, editors) are never
; prompted to close.
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=yes
RestartIfNeededByRun=no
SetupLogging=yes
UninstallDisplayIcon={app}\manager\{#MyAppExeName}

[Registry]
; 开机静默自动启动输入法后台核心服务
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PersonalIMEService"; ValueData: """{app}\ime-service.exe"""; Flags: uninsdeletevalue dontcreatekey
Root: HKLM; Subkey: "Software\PersonalIME"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式 (Personal IME 管理器)"; GroupDescription: "快捷方式:"; Flags: unchecked

[Files]
; 1. TSF In-Proc COM Server DLL (x64 registered explicitly in [Code] inside versioned directory)
; Installing into versions\<ver>\ avoids overwriting in-use DLLs in running host apps.
; Explicit registration avoids Inno Setup's shared-CLSID regserver rollback/uninstall hazards.
Source: "..\build\Release\PersonalIME.dll"; DestDir: "{code:GetVersionDir}"; Flags: 64bit; AfterInstall: RegisterVersionedDll

; 2. Background Core IME Service
Source: "..\build\Release\ime-service.exe"; DestDir: "{app}"; Flags: 64bit restartreplace

; 3. Maintenance CLI
Source: "..\build\Release\ime-cli.exe"; DestDir: "{app}"; Flags: 64bit restartreplace

; 4. Self-Contained WPF Desktop Manager (.NET 10 x64)
Source: "..\out\publish\manager\*"; DestDir: "{app}\manager"; Flags: 64bit recursesubdirs createallsubdirs restartreplace

; 5. High-Speed Precompiled Binary Dictionaries & Attribution Notices
Source: "..\data\dictionaries\pinyin.dict"; DestDir: "{app}\dictionaries"; Flags: 64bit ignoreversion
Source: "..\data\dictionaries\wubi86.dict"; DestDir: "{app}\dictionaries"; Flags: 64bit ignoreversion
Source: "..\data\dictionaries\NOTICE.txt"; DestDir: "{app}\dictionaries"; Flags: ignoreversion

; 6. Legal & Documentation
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Personal IME 管理器"; Filename: "{app}\manager\{#MyAppExeName}"
Name: "{group}\启动后台服务"; Filename: "{app}\{#MyServiceExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Personal IME 管理器"; Filename: "{app}\manager\{#MyAppExeName}"; Tasks: desktopicon

[UninstallDelete]
; Clean up dynamically generated dictionaries, cache, logs, versioned DLLs, and empty directories
Type: filesandordirs; Name: "{app}\versions"
Type: files; Name: "{app}\PersonalIME.dll"
Type: filesandordirs; Name: "{app}\dictionaries"
Type: filesandordirs; Name: "{app}\manager"
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\*.tmp"
Type: files; Name: "{app}\*.db*"
Type: filesandordirs; Name: "{app}"

[Run]
; Launch service and manager under original user session context unless reboot pending
Filename: "{app}\{#MyServiceExeName}"; Description: "立即启动后台输入服务"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: NotRestarting
Filename: "{app}\manager\{#MyAppExeName}"; Description: "打开 Personal IME 管理器"; Flags: nowait postinstall skipifsilent unchecked runasoriginaluser; Check: NotRestarting

[Code]
#include "registry_snapshot.iss"

var
  G_InitialChecksum: String;
  G_RestartNeeded: Boolean;
  G_PurgeUserData: Boolean;
  G_ActiveVersionDir: String;
  G_TempUnregDllPath: String;
  G_PrevRegisteredDll: String;
  G_PrevHklmClsidDefault: TRegValueBackup;
  G_PrevHkcuClsidDefault: TRegValueBackup;
  G_PrevHklmInproc: TRegValueBackup;
  G_PrevHkcuInproc: TRegValueBackup;
  G_PrevHklmThreading: TRegValueBackup;
  G_PrevHkcuThreading: TRegValueBackup;
  G_PrevIconFile: TRegValueBackup;
  G_PrevHklmDesc: TRegValueBackup;
  G_PrevHklmDisplayDesc: TRegValueBackup;
  G_PrevHkcuDesc: TRegValueBackup;
  G_PrevHkcuDisplayDesc: TRegValueBackup;
  G_RegistrationNeedsRestore: Boolean;
  G_InstallSucceeded: Boolean;

function GetVersionDir(Param: String): String;
var
  BaseDir, Candidate: String;
  Suffix: Integer;
begin
  if G_ActiveVersionDir <> '' then
  begin
    Result := G_ActiveVersionDir;
    exit;
  end;

  BaseDir := ExpandConstant('{app}\versions');
  Candidate := BaseDir + '\{#MyAppVersion}';

  // Pure, side-effect-free directory selection: if the target version directory
  // already exists (e.g. same-version reinstall), allocate a fresh suffix.
  if DirExists(Candidate) or FileExists(Candidate + '\PersonalIME.dll') then
  begin
    Suffix := 1;
    while DirExists(Candidate + '-' + IntToStr(Suffix)) do
      Suffix := Suffix + 1;
    Candidate := Candidate + '-' + IntToStr(Suffix);
  end;

  G_ActiveVersionDir := Candidate;
  Result := G_ActiveVersionDir;
end;

function GetRegisteredDllPathRaw(): String;
var
  RegPath: String;
begin
  Result := '';
  RegPath := 'CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\InProcServer32';
  if RegQueryStringValue(HKEY_CLASSES_ROOT, RegPath, '', Result) and (Result <> '') then
    exit;
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + RegPath, '', Result) and (Result <> '') then
    exit;
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Classes\' + RegPath, '', Result) and (Result <> '') then
    exit;
  Result := '';
end;

function GetRegisteredDllPath(): String;
begin
  Result := GetRegisteredDllPathRaw();
  if Result = '' then
    Result := ExpandConstant('{app}\PersonalIME.dll');
end;

procedure CleanupStaleVersions();
var
  VersionsDir, OldDir, ActiveComDll, LegacyDll: String;
  FindRec: TFindRec;
begin
  ActiveComDll := GetRegisteredDllPath();

  // Best-effort cleanup of legacy pre-1.1.6 root DLL if no longer active or locked
  LegacyDll := ExpandConstant('{app}\PersonalIME.dll');
  if FileExists(LegacyDll) and (CompareText(LegacyDll, ActiveComDll) <> 0) then
    DeleteFile(LegacyDll);

  VersionsDir := ExpandConstant('{app}\versions');
  if not DirExists(VersionsDir) then
    exit;

  if FindFirst(VersionsDir + '\*', FindRec) then
  begin
    try
      repeat
        if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and
           (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          OldDir := VersionsDir + '\' + FindRec.Name;
          if (CompareText(OldDir, G_ActiveVersionDir) <> 0) and
             (CompareText(OldDir + '\PersonalIME.dll', ActiveComDll) <> 0) then
          begin
            // Silently skip if an older host process still locks this DLL;
            // it will be cleaned up on a future install or uninstall.
            DelTree(OldDir, True, True, True);
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  G_PurgeUserData := False;
  if not UninstallSilent() then
  begin
    if MsgBox('是否同时删除个人词库与自定义配置数据？' + #13#10 + #13#10 +
              '【是 (Y)】：彻底清除所有用户数据、词库数据库与配置记录（%APPDATA%\PersonalIME）。' + #13#10 +
              '【否 (N)】：保留个人词库（推荐，方便后续重新安装时自动恢复使用）。',
              mbConfirmation, MB_YESNO) = IDYES then
    begin
      G_PurgeUserData := True;
    end;
  end;
end;

procedure InitializeWizard();
begin
  G_InitialChecksum := MakePendingFileRenameOperationsChecksum;
  G_RestartNeeded := False;
end;

procedure BackupPreviousRegistration();
var
  ClsidKey, InprocKey, TipProfileKey: String;
begin
  G_PrevRegisteredDll := GetRegisteredDllPathRaw();
  ClsidKey := 'CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}';
  InprocKey := ClsidKey + '\InProcServer32';
  TipProfileKey := 'SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}';

  G_PrevHklmClsidDefault := BackupRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + ClsidKey, '');
  G_PrevHkcuClsidDefault := BackupRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + ClsidKey, '');
  G_PrevHklmInproc := BackupRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + InprocKey, '');
  G_PrevHkcuInproc := BackupRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + InprocKey, '');
  G_PrevHklmThreading := BackupRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + InprocKey, 'ThreadingModel');
  G_PrevHkcuThreading := BackupRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + InprocKey, 'ThreadingModel');
  G_PrevIconFile := BackupRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'IconFile');
  G_PrevHklmDesc := BackupRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'Description');
  G_PrevHklmDisplayDesc := BackupRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'Display Description');
  G_PrevHkcuDesc := BackupRegValue(HKEY_CURRENT_USER, TipProfileKey, 'Description');
  G_PrevHkcuDisplayDesc := BackupRegValue(HKEY_CURRENT_USER, TipProfileKey, 'Display Description');
end;

function RestorePreviousRegistration(): Boolean;
var
  ResultCode: Integer;
  ClsidKey, InprocKey, TipProfileKey, NewDllPath, UnregTarget: String;
begin
  Result := True;
  ClsidKey := 'CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}';
  InprocKey := ClsidKey + '\InProcServer32';
  TipProfileKey := 'SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}';

  if G_PrevHklmInproc.Exists or G_PrevHkcuInproc.Exists then
  begin
    // Directly restore saved registry metadata without invoking old DLL's DllRegisterServer
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + ClsidKey, '', G_PrevHklmClsidDefault) then
      Result := False;
    if not RestoreRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + ClsidKey, '', G_PrevHkcuClsidDefault) then
      Result := False;
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + InprocKey, '', G_PrevHklmInproc) then
      Result := False;
    if not RestoreRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + InprocKey, '', G_PrevHkcuInproc) then
      Result := False;
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + InprocKey, 'ThreadingModel', G_PrevHklmThreading) then
      Result := False;
    if not RestoreRegValue(HKEY_CURRENT_USER, 'Software\Classes\' + InprocKey, 'ThreadingModel', G_PrevHkcuThreading) then
      Result := False;
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'IconFile', G_PrevIconFile) then
      Result := False;
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'Description', G_PrevHklmDesc) then
      Result := False;
    if not RestoreRegValue(HKEY_LOCAL_MACHINE, TipProfileKey, 'Display Description', G_PrevHklmDisplayDesc) then
      Result := False;
    if not RestoreRegValue(HKEY_CURRENT_USER, TipProfileKey, 'Description', G_PrevHkcuDesc) then
      Result := False;
    if not RestoreRegValue(HKEY_CURRENT_USER, TipProfileKey, 'Display Description', G_PrevHkcuDisplayDesc) then
      Result := False;
  end
  else
  begin
    // Fresh install failure cleanup: use remaining DLL or independent {tmp} copy
    UnregTarget := '';
    if G_ActiveVersionDir <> '' then
    begin
      NewDllPath := G_ActiveVersionDir + '\PersonalIME.dll';
      if FileExists(NewDllPath) then
        UnregTarget := NewDllPath;
    end;
    if (UnregTarget = '') and (G_TempUnregDllPath <> '') and FileExists(G_TempUnregDllPath) then
      UnregTarget := G_TempUnregDllPath;

    if UnregTarget <> '' then
    begin
      Result := Exec('regsvr32.exe', '/u /s "' + UnregTarget + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
      // Some historical unregister implementations returned S_OK even when
      // individual operations failed. Verify fresh-install cleanup explicitly.
      if RegKeyExists(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\' + ClsidKey) or
         RegKeyExists(HKEY_CURRENT_USER, 'Software\Classes\' + ClsidKey) or
         RegKeyExists(HKEY_LOCAL_MACHINE, TipProfileKey) or
         RegKeyExists(HKEY_CURRENT_USER, TipProfileKey) then
        Result := False;
    end
    else
      Result := False;
  end;
end;

procedure RegisterVersionedDll();
var
  ResultCode: Integer;
  NewDllPath: String;
begin
  NewDllPath := ExpandConstant(CurrentFileName);
  // Keep an independent copy in {tmp} so fresh-install rollback can still unregister
  // COM/TSF entries even after Inno Setup deletes the rolled-back target DLL.
  G_TempUnregDllPath := ExpandConstant('{tmp}\PersonalIME.unreg.dll');
  if not CopyFile(NewDllPath, G_TempUnregDllPath, False) then
    RaiseException('无法准备输入法注册回退副本，安装已停止。');

  G_RegistrationNeedsRestore := True;

  if (not Exec('regsvr32.exe', '/s "' + NewDllPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    if RestorePreviousRegistration() then
    begin
      G_RegistrationNeedsRestore := False;
      RaiseException('注册输入法组件失败 (regsvr32 exit code ' + IntToStr(ResultCode) + ')，已恢复原有注册配置。');
    end
    else
      RaiseException('注册输入法组件失败 (regsvr32 exit code ' + IntToStr(ResultCode) + ')，且恢复原有注册配置失败，请重新运行安装程序修复。');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Installation is now committed; safe to mark success and clean up stale versions.
    G_InstallSucceeded := True;
    CleanupStaleVersions();
    if G_InitialChecksum <> MakePendingFileRenameOperationsChecksum then
      G_RestartNeeded := True;
  end;
end;

procedure DeinitializeSetup();
begin
  if G_RegistrationNeedsRestore and (not G_InstallSucceeded) then
  begin
    if not RestorePreviousRegistration() then
    begin
      Log('ERROR: Failed to restore previous PersonalIME registration during setup rollback.');
      SuppressibleMsgBox('安装未完成，且恢复原有输入法注册状态失败，请重新运行安装程序修复。', mbError, MB_OK, IDOK);
    end;
  end;
  if (G_TempUnregDllPath <> '') and FileExists(G_TempUnregDllPath) then
    DeleteFile(G_TempUnregDllPath);
end;

function NotRestarting(): Boolean;
begin
  Result := not G_RestartNeeded;
end;

function NeedRestart(): Boolean;
begin
  Result := G_RestartNeeded;
end;

// Stop running PersonalIME processes confirmed to belong to this installation directory only
procedure StopAllAppProcesses();
var
  ResultCode: Integer;
  AppPath, PsScript: String;
begin
  AppPath := ExpandConstant('{app}');
  StringChangeEx(AppPath, '''', '''''', True);
  PsScript :=
    '$prefix = ''' + AppPath + '\''; ' +
    '$targets = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | ' +
    'Where-Object { ($_.Name -ieq ''ime-service.exe'' -or $_.Name -ieq ''PersonalIME.Manager.exe'') -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) }; ' +
    'foreach ($t in $targets) { ' +
    '  if ($t.Name -ieq ''PersonalIME.Manager.exe'') { ' +
    '    $p = Get-Process -Id $t.ProcessId -ErrorAction SilentlyContinue; ' +
    '    if ($p) { $null = $p.CloseMainWindow() } ' +
    '  } ' +
    '}; ' +
    'Start-Sleep -Milliseconds 300; ' +
    'foreach ($t in $targets) { ' +
    '  Stop-Process -Id $t.ProcessId -Force -ErrorAction SilentlyContinue ' +
    '}';
  Exec('powershell.exe', '-NoProfile -Command "' + PsScript + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(300);
end;

// Deep cleanup for machine and user registry keys
procedure CleanResidualRegistry();
var
  ResultCode: Integer;
begin
  // 1. Clean HKLM CTF TIP, COM Registration, and PersonalIME InstallDir
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_CLASSES_ROOT, 'CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteValue(HKEY_LOCAL_MACHINE, 'Software\Microsoft\Windows\CurrentVersion\Run', 'PersonalIMEService');
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'Software\PersonalIME');

  // 2. Clean HKCU CTF TIP and COM Registration for current user context
  RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');

  // 3. Remove PersonalIME GUID from HKCU CTF SortOrder AssemblyItem
  Exec('powershell.exe', '-NoProfile -Command "Get-ChildItem -Path ''HKCU:\Software\Microsoft\CTF\SortOrder\AssemblyItem'' -Recurse -ErrorAction SilentlyContinue | Where-Object { (Get-ItemProperty -Path $_.PsPath -Name ''CLSID'' -ErrorAction SilentlyContinue).CLSID -eq ''{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}'' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  BackupPreviousRegistration();
  G_RegistrationNeedsRestore := False;
  G_InstallSucceeded := False;
  StopAllAppProcesses();
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  UserDataDir, RegisteredDll: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. Terminate all related processes first
    StopAllAppProcesses();

    // 2. Explicitly unregister COM / TSF profile before file deletion
    RegisteredDll := GetRegisteredDllPath();
    if FileExists(RegisteredDll) then
      Exec('regsvr32.exe', '/u /s "' + RegisteredDll + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    // 3. Deep Registry Cleanup
    CleanResidualRegistry();

    // 4. Clean user data if confirmed by user
    if G_PurgeUserData then
    begin
      UserDataDir := ExpandConstant('{userappdata}\PersonalIME');
      if DirExists(UserDataDir) then
      begin
        DelTree(UserDataDir, True, True, True);
      end;
    end;
  end;
end;
