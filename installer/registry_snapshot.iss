// Raw registry value snapshots shared by Setup and the isolated regression harness.
// PersonalIME is x64-only: native calls must explicitly use the 64-bit view,
// even when the Inno Setup executable itself is 32-bit.
type
  TRegValueBackup = record
    Exists: Boolean;
    ValueType: Cardinal;
    DataSize: Cardinal;
    RawData: String;
  end;

function SnapshotOpenKey(RootKey: Integer; SubKey: String; Options, Access: Cardinal; var Key: THandle): Longint;
  external 'RegOpenKeyExW@advapi32.dll stdcall';
function SnapshotCreateKey(RootKey: Integer; SubKey: String; Reserved: Cardinal; ClassName: Integer; Options, Access: Cardinal; Security: Integer; var Key: THandle; Disposition: Integer): Longint;
  external 'RegCreateKeyExW@advapi32.dll stdcall';
function SnapshotQuerySize(Key: THandle; ValueName: String; Reserved: Integer; var ValueType: Cardinal; Data: Integer; var DataSize: Cardinal): Longint;
  external 'RegQueryValueExW@advapi32.dll stdcall';
function SnapshotQueryData(Key: THandle; ValueName: String; Reserved: Integer; var ValueType: Cardinal; Data: String; var DataSize: Cardinal): Longint;
  external 'RegQueryValueExW@advapi32.dll stdcall';
function SnapshotSetData(Key: THandle; ValueName: String; Reserved, ValueType: Cardinal; Data: String; DataSize: Cardinal): Longint;
  external 'RegSetValueExW@advapi32.dll stdcall';
function SnapshotDeleteValue(Key: THandle; ValueName: String): Longint;
  external 'RegDeleteValueW@advapi32.dll stdcall';
function SnapshotCloseKey(Key: THandle): Longint;
  external 'RegCloseKey@advapi32.dll stdcall';

function BackupRegValue(RootKey: Integer; const SubKey, ValueName: String): TRegValueBackup;
var
  Key: THandle;
  Error: Longint;
  Capacity: Cardinal;
begin
  Result.Exists := False;
  Result.ValueType := 0;
  Result.DataSize := 0;
  Result.RawData := '';
  Error := SnapshotOpenKey(RootKey, SubKey, 0, $0101, Key);
  if Error = 2 then exit;
  if Error <> 0 then
    RaiseException('Cannot snapshot registry key ' + SubKey + ': ' + IntToStr(Error));
  try
    Error := SnapshotQuerySize(Key, ValueName, 0, Result.ValueType, 0, Result.DataSize);
    if Error = 2 then exit;
    if Error <> 0 then
      RaiseException('Cannot snapshot registry value ' + SubKey + ': ' + IntToStr(Error));
    if Result.DataSize > 1048576 then
      RaiseException('Registry value exceeds snapshot limit: ' + SubKey);
    Capacity := Result.DataSize;
    // Keep one extra UTF-16 unit; DataSize preserves odd-length binary and embedded NULs.
    Result.RawData := StringOfChar(#0, (Capacity + 1) div 2 + 1);
    Error := SnapshotQueryData(Key, ValueName, 0, Result.ValueType, Result.RawData, Result.DataSize);
    if (Error <> 0) or (Result.DataSize <> Capacity) then
      RaiseException('Registry value changed or could not be read: ' + SubKey);
    Result.Exists := True;
  finally
    SnapshotCloseKey(Key);
  end;
end;

function RestoreRegValue(RootKey: Integer; const SubKey, ValueName: String; const Backup: TRegValueBackup): Boolean;
var
  Key: THandle;
  Error: Longint;
begin
  if Backup.Exists then
  begin
    Error := SnapshotCreateKey(RootKey, SubKey, 0, 0, 0, $0102, 0, Key, 0);
    if Error <> 0 then begin Log('Registry restore open/create failed: ' + IntToStr(Error)); Result := False; exit; end;
    try
      Result := SnapshotSetData(Key, ValueName, 0, Backup.ValueType, Backup.RawData, Backup.DataSize) = 0;
    finally
      SnapshotCloseKey(Key);
    end;
  end
  else
  begin
    Error := SnapshotOpenKey(RootKey, SubKey, 0, $0102, Key);
    if Error = 2 then begin Result := True; exit; end;
    if Error <> 0 then begin Log('Registry restore open/create failed: ' + IntToStr(Error)); Result := False; exit; end;
    try
      Error := SnapshotDeleteValue(Key, ValueName);
      Result := (Error = 0) or (Error = 2);
    finally
      SnapshotCloseKey(Key);
    end;
  end;
end;
