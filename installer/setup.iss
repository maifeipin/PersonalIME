; =====================================================================
; Personal IME (私人输入法) - Inno Setup 6 Installation Script
; Stage 5 Production-Grade Machine Installer
; Target Architecture: Windows 10/11 x64 ONLY
; =====================================================================

#ifndef MyAppVersion
#define MyAppVersion "1.0.0"
#endif

#define MyAppName "Personal IME"
#define MyAppPublisher "Personal IME Project"
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

; Clean Machine-Level Privileges Model:
; Admin privileges used strictly for Program Files & Machine COM Registration.
; User data (%APPDATA%) and user profile registry are untouched by admin installer.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

; Safety & In-Use Handling:
; Never forcefully kill third-party host applications (Explorer, browsers, terminals).
; Locked DLLs use Windows MoveFileEx delay until system restart.
CloseApplications=no
RestartIfNeededByRun=no
SetupLogging=yes
UninstallDisplayIcon={app}\manager\{#MyAppExeName}

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

[Run]
; Launch service and manager under original user session context unless reboot pending
Filename: "{app}\{#MyServiceExeName}"; Description: "立即启动后台输入服务"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: NotRestarting
Filename: "{app}\manager\{#MyAppExeName}"; Description: "打开 Personal IME 管理器"; Flags: nowait postinstall skipifsilent unchecked runasoriginaluser; Check: NotRestarting

[Code]
var
  G_RestartNeeded: Boolean;

function NotRestarting(): Boolean;
begin
  Result := not G_RestartNeeded;
end;

function NeedRestart(): Boolean;
begin
  Result := G_RestartNeeded;
end;
// Stop any currently running ime-service before installing, upgrading or uninstalling
procedure StopAppServiceIfRunning();
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/F /IM {#MyServiceExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(200);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopAppServiceIfRunning();
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopAppServiceIfRunning();
  end;
end;
