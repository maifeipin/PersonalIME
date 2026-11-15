# Versioned TSF DLL installation

The installer writes `PersonalIME.dll` into a new `versions/<version>[-N]`
directory. It never deletes an existing DLL while selecting a destination.
The DLL's `AfterInstall` callback registers the new path while Inno can still
roll back the file installation. Old directories are cleaned only after the
installation reaches `ssPostInstall`.

Before changing registration, Setup snapshots the machine and current user's
COM values and the profile metadata it changes. Snapshots preserve the native
registry type and raw bytes, including REG_EXPAND_SZ, embedded NULs and binary
values. Read errors abort setup rather than masquerading as absent values.
All native snapshot operations explicitly address the 64-bit registry view.

The temporary unregister DLL must be copied successfully before registration
is attempted. This copy allows first-install cleanup after automatic file
rollback has removed the installed DLL. Failed registration restoration is
retried when Setup exits and reported in the installation log. A successful
unregister exit code is also checked against remaining COM/profile keys.

## Automated verification

```powershell
./scripts/test_installer_registry.ps1
```

This compiles a small Inno harness using the production
`registry_snapshot.iss`. It runs silently, exits before the installation phase,
and creates/removes only a disposable
`HKCU\Software\PersonalIMEInstallerTests` child key. It tests absent values,
REG_SZ, REG_EXPAND_SZ, odd-length REG_BINARY, REG_DWORD, REG_MULTI_SZ,
REG_QWORD and zero-length REG_NONE. It does not register or install PersonalIME.
The result marker and log are saved under `out/installer-tests`.

## Release acceptance in a disposable Windows VM

Compilation and the isolated harness do not validate TSF installation behavior.
Before publishing a release, verify the following in a VM snapshot:

1. Upgrade installed 1.1.4/1.1.5 while Notepad and a Chromium/Electron host
   retain the old DLL. Keep those hosts alive; verify a genuinely new host
   loads the new registered path. Existing hosts need not switch versions.
2. Reinstall the same version. Verify a new suffix directory is chosen and
   cancellation leaves the original registration and files intact.
3. Inject registration failure, then a failure after successful registration
   but before installation commit. Compare HKLM/HKCU COM values and profile
   metadata before/after, including value types and missing values. For an
   upgrade, verify a disabled input profile remains disabled and its order is
   preserved; no old DLL registration entry point should run during recovery.
4. Repeat the latter failure on a fresh machine with no previous registration.
   Verify cleanup succeeds even after the installed DLL is removed, leaving
   no PersonalIME COM/profile/category registration.
5. Run service/Manager instances from another directory. Installing or
   uninstalling this copy must leave those processes alive. Test a custom
   installation path, including spaces and an apostrophe.
6. Uninstall after two upgrades, with an old DLL still loaded. Verify current
   registration is removed and user data is retained unless explicitly chosen
   for deletion. In-use old files may remain until a later cleanup.

Use VM snapshots to recover from injected failures. Do not perform failure
injection against the development machine's active input method.
