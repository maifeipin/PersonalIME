; =====================================================================
; 人人 - Inno Setup 6 Installation Script
; Stage 5 Production-Grade Machine Installer
; Target Architecture: Windows 10/11 x64 ONLY
; =====================================================================

#ifndef MyAppVersion
#define MyAppVersion "1.0.9"
#endif

#define MyAppName "人人"
#define MyAppPublisher "人人 项目组"
#define MyAppURL "https://github.com/maifeipin/PersonalIME"
#define MyAppExeName "PersonalIME.Manager.exe"
#define MyServiceExeName "ime-service.exe"

[Setup]
AppId={{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion} (x64)
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
VersionInfoVersion={#MyAppVersion}.0
VersionInfoTextVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 输入法安装程序
VersionInfoCopyright=Copyright (C) 2026 {#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

; Clean Machine-Level Privileges Model:
; Admin privileges used strictly for Program Files & Machine COM Registration.
; User data (%APPDATA%) and user profile registry are untouched by admin installer.
PrivilegesRequired=admin

; Safety & Clean In-Use Handling:
; Enable Windows Restart Manager to detect and safely close locking processes (such as Explorer),
; allowing PersonalIME.dll to be cleanly deleted without requiring a system reboot.
CloseApplications=yes
RestartApplications=yes
RestartIfNeededByRun=no
SetupLogging=yes
UninstallDisplayIcon={app}\manager\{#MyAppExeName}

[Registry]
; 开机静默自动启动输入法后台核心服务
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PersonalIMEService"; ValueData: """{app}\ime-service.exe"""; Flags: uninsdeletevalue dontcreatekey

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式 (Personal IME 管理器)"; GroupDescription: "快捷方式:"; Flags: unchecked

[Files]
; 1. TSF In-Proc COM Server DLL (x64 registered into TSF)
; restartreplace ensures in-use DLLs locked by host apps are scheduled for replacement on reboot
Source: "..\build\Release\PersonalIME.dll"; DestDir: "{app}"; Flags: 64bit regserver restartreplace uninsrestartdelete

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
; Clean up dynamically generated dictionaries, cache, logs, and empty directories
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
var
  G_InitialChecksum: String;
  G_RestartNeeded: Boolean;
  G_PurgeUserData: Boolean;

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

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if G_InitialChecksum <> MakePendingFileRenameOperationsChecksum then
      G_RestartNeeded := True;
  end;
end;

function NotRestarting(): Boolean;
begin
  Result := not G_RestartNeeded;
end;

function NeedRestart(): Boolean;
begin
  Result := G_RestartNeeded;
end;

// Stop all running PersonalIME processes (ime-service and PersonalIME.Manager)
procedure StopAllAppProcesses();
var
  ResultCode: Integer;
  AppPath: String;
begin
  AppPath := ExpandConstant('{app}');
  // 1. Terminate processes belonging to this installation
  Exec('powershell.exe', '-NoProfile -Command "Get-Process -Name ime-service, PersonalIME.Manager -ErrorAction SilentlyContinue | Where-Object { try { $_.MainModule.FileName -like ''' + AppPath + '\*'' } catch { $true } } | Stop-Process -Force"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // 2. Also run taskkill for lingering processes
  Exec('taskkill.exe', '/F /IM ime-service.exe /IM PersonalIME.Manager.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(300);
end;

// Deep cleanup for machine and user registry keys
procedure CleanResidualRegistry();
var
  ResultCode: Integer;
begin
  // 1. Clean HKLM CTF TIP and COM Registration
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'SOFTWARE\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_CLASSES_ROOT, 'CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteValue(HKEY_LOCAL_MACHINE, 'Software\Microsoft\Windows\CurrentVersion\Run', 'PersonalIMEService');

  // 2. Clean HKCU CTF TIP and COM Registration for current user context
  RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');
  RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}');

  // 3. Remove PersonalIME GUID from HKCU CTF SortOrder AssemblyItem
  Exec('powershell.exe', '-NoProfile -Command "Get-ChildItem -Path ''HKCU:\Software\Microsoft\CTF\SortOrder\AssemblyItem'' -Recurse -ErrorAction SilentlyContinue | Where-Object { (Get-ItemProperty -Path $_.PsPath -Name ''CLSID'' -ErrorAction SilentlyContinue).CLSID -eq ''{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}'' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopAllAppProcesses();
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  UserDataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. Terminate all related processes first
    StopAllAppProcesses();

    // 2. Explicitly unregister COM / TSF profile before file deletion
    Exec('regsvr32.exe', '/u /s "' + ExpandConstant('{app}\PersonalIME.dll') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
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
