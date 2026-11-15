; Executes shared production snapshot code against a disposable HKCU test key only.
[Setup]
AppName=PersonalIME registry snapshot regression
AppVersion=1.0
DefaultDirName={tmp}\PersonalIMERegistryTest
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputDir=..\out\installer-tests
OutputBaseFilename=test_installer_registry
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
SetupLogging=yes

[Code]
#include "..\installer\registry_snapshot.iss"

procedure Require(Condition: Boolean; Message: String);
begin
  if not Condition then RaiseException(Message);
end;

procedure CheckRawRoundTrip(const TestKey: String; ValueType: Cardinal; const RawData: String; DataSize: Cardinal);
var
  Original, Saved, ReadBack, Missing: TRegValueBackup;
begin
  Original.Exists := True;
  Original.ValueType := ValueType;
  Original.RawData := RawData;
  Original.DataSize := DataSize;
  Require(RestoreRegValue(HKEY_CURRENT_USER, TestKey, 'value', Original), 'Seed failed');
  Saved := BackupRegValue(HKEY_CURRENT_USER, TestKey, 'value');
  Require(Saved.Exists and (Saved.ValueType = ValueType) and (Saved.DataSize = DataSize), 'Snapshot type/size changed');
  Require(Copy(Saved.RawData, 1, (DataSize + 1) div 2) = Copy(RawData, 1, (DataSize + 1) div 2), 'Snapshot bytes changed');
  Require(RegWriteStringValue(HKEY_CURRENT_USER, TestKey, 'value', 'new version'), 'Overwrite failed');
  Require(RestoreRegValue(HKEY_CURRENT_USER, TestKey, 'value', Saved), 'Restore failed');
  ReadBack := BackupRegValue(HKEY_CURRENT_USER, TestKey, 'value');
  Require((ReadBack.ValueType = ValueType) and (ReadBack.DataSize = DataSize), 'Restored type/size changed');
  Require(ReadBack.RawData = Saved.RawData, 'Restored bytes changed');
  Missing.Exists := False;
  Require(RestoreRegValue(HKEY_CURRENT_USER, TestKey, 'value', Missing), 'Delete newly created value failed');
  Require(not RegValueExists(HKEY_CURRENT_USER, TestKey, 'value'), 'New value remained');
end;

function InitializeSetup(): Boolean;
var
  TestKey, Marker: String;
  Missing: TRegValueBackup;
begin
  Log('Starting isolated registry regression');
  Result := False; // Harness never enters the installation phase.
  TestKey := 'Software\PersonalIMEInstallerTests\' + GetDateTimeString('yyyymmddhhnnss', '-', ':');
  Marker := ExpandConstant('{param:RESULTFILE}');
  try
    try
      Missing := BackupRegValue(HKEY_CURRENT_USER, TestKey, 'missing');
      Require(not Missing.Exists, 'Absent value was not absent');
      Require(RestoreRegValue(HKEY_CURRENT_USER, TestKey, 'missing', Missing), 'Absent restore failed');
      CheckRawRoundTrip(TestKey, 1, 'old.dll' + #0, 16); // REG_SZ
      CheckRawRoundTrip(TestKey, 2, '%TEMP%\old.dll' + #0, 30); // REG_EXPAND_SZ, unexpanded
      CheckRawRoundTrip(TestKey, 3, #$0201#$0003, 3); // odd-length REG_BINARY
      CheckRawRoundTrip(TestKey, 4, #$5678#$1234, 4); // REG_DWORD
      CheckRawRoundTrip(TestKey, 7, 'a' + #0 + 'b' + #0#0, 10); // REG_MULTI_SZ
      CheckRawRoundTrip(TestKey, 11, #$3210#$7654#$BA98#$FEDC, 8); // REG_QWORD
      CheckRawRoundTrip(TestKey, 0, '', 0); // zero-length REG_NONE
      Require(SaveStringToFile(Marker, 'PASS: absent values and 7 raw registry types round-trip exactly', False), 'Cannot write test result');
    except
      SaveStringToFile(Marker, 'FAIL: ' + GetExceptionMessage, False);
    end;
  finally
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, TestKey);
  end;
end;
