using System.Diagnostics;
using System.IO;
using System.Text.Json;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class RealServiceIntegrationTests
{
    private static Process? s_serviceProcess;
    private static string s_testPipeName = string.Empty;
    private static string s_testDbPath = string.Empty;
    private static string s_exportFilePath = string.Empty;

    private static string FindRepoRoot()
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            if (File.Exists(Path.Combine(dir, "CMakeLists.txt")) && Directory.Exists(Path.Combine(dir, "src", "ime_service")))
            {
                return dir;
            }
            dir = Path.GetDirectoryName(dir);
        }
        return @"C:\Projects\AITools\PersonalIME";
    }

    [ClassInitialize]
    public static void ClassInit(TestContext context)
    {
        string repoRoot = FindRepoRoot();
        int pid = Process.GetCurrentProcess().Id;
        s_testPipeName = $"PersonalIME_Admin_E2E_{pid}";
        s_testDbPath = Path.Combine(repoRoot, $"out/admin_e2e_test_{pid}.db");
        s_exportFilePath = Path.Combine(repoRoot, $"out/admin_e2e_export_{pid}.json");

        if (File.Exists(s_testDbPath)) File.Delete(s_testDbPath);
        if (File.Exists(s_exportFilePath)) File.Delete(s_exportFilePath);
        Directory.CreateDirectory(Path.Combine(repoRoot, "out"));

        // Locate ime-service.exe
        string serviceExe = Path.Combine(repoRoot, @"build\Release\ime-service.exe");
        if (!File.Exists(serviceExe))
        {
            Assert.Inconclusive("ime-service.exe not found at " + serviceExe);
            return;
        }

        string dictDir = Path.Combine(repoRoot, @"data\dictionaries");

        // Start isolated ime-service
        var psi = new ProcessStartInfo
        {
            FileName = serviceExe,
            Arguments = $"--db \"{s_testDbPath}\" --admin-pipe \"{s_testPipeName}\" --dict \"{dictDir}\"",
            WorkingDirectory = repoRoot,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        s_serviceProcess = Process.Start(psi);
        Assert.IsNotNull(s_serviceProcess, "Failed to start isolated ime-service process");

        // Wait briefly for server pipe to be created
        Thread.Sleep(800);
    }

    [ClassCleanup]
    public static void ClassCleanup()
    {
        if (s_serviceProcess != null && !s_serviceProcess.HasExited)
        {
            s_serviceProcess.Kill();
            s_serviceProcess.WaitForExit(3000);
            s_serviceProcess.Dispose();
            s_serviceProcess = null;
        }

        try
        {
            if (File.Exists(s_testDbPath)) File.Delete(s_testDbPath);
            if (File.Exists(s_exportFilePath)) File.Delete(s_exportFilePath);
        }
        catch
        {
            // Best effort cleanup
        }
    }

    [TestMethod]
    public async Task TestRealServiceFullWorkflowIntegration()
    {
        if (s_serviceProcess == null) return;

        var client = new AdminIpcClient(s_testPipeName);

        // 1. GetStatus
        var status = await client.GetStatusAsync();
        Assert.IsFalse(string.IsNullOrEmpty(status.ServiceVersion));
        Assert.IsFalse(string.IsNullOrEmpty(status.DatabasePath), "status.DatabasePath should be populated from db_path");

        // 2. Wordbook: AddEntry with leading/trailing spaces and newlines
        string textWithEdges = "  edges and newline\r\n";
        var entryIdEdges = await client.AddEntryAsync(textWithEdges, [
            new EntryCodeItem { Scheme = InputSchemes.Custom, Code = ";edges" }
        ]);
        Assert.IsFalse(string.IsNullOrEmpty(entryIdEdges));
        var entriesEdges = await client.GetEntriesAsync(";edges", null, 0, 10);
        Assert.AreEqual(1, entriesEdges.Total);
        Assert.AreEqual(textWithEdges, entriesEdges.Entries[0].Text, "Entry text must preserve leading/trailing whitespace exactly");

        var entryId = await client.AddEntryAsync("古都西安", [
            new EntryCodeItem { Scheme = InputSchemes.Pinyin, Code = "xian" },
            new EntryCodeItem { Scheme = InputSchemes.Wubi86, Code = "sq" }
        ]);
        Assert.IsFalse(string.IsNullOrEmpty(entryId));

        // Query entries
        var entries = await client.GetEntriesAsync("西安", null, 0, 10);
        Assert.IsTrue(entries.Total >= 1);
        Assert.AreEqual("古都西安", entries.Entries[0].Text);

        // Update entry
        await client.UpdateEntryAsync(entryId, "十三朝古都西安", null);
        var entriesUpdated = await client.GetEntriesAsync("古都", null, 0, 10);
        Assert.AreEqual("十三朝古都西安", entriesUpdated.Entries[0].Text);

        // 3. Sorting & Pinned: SetPin and ReorderPinned with Concurrency Revision
        var p1 = await client.AddEntryAsync("置顶条目甲", [new EntryCodeItem { Scheme = InputSchemes.Pinyin, Code = "testpin" }]);
        var p2 = await client.AddEntryAsync("置顶条目乙", [new EntryCodeItem { Scheme = InputSchemes.Pinyin, Code = "testpin" }]);

        ulong rev1 = await client.SetPinAsync(InputSchemes.Pinyin, "testpin", p1, true, 1);
        Assert.IsTrue(rev1 >= 1);

        ulong rev2 = await client.SetPinAsync(InputSchemes.Pinyin, "testpin", p2, true, 2, expectedRevision: rev1);
        Assert.AreEqual(rev1 + 1, rev2);

        // GetPinned
        var pinned = await client.GetPinnedAsync(InputSchemes.Pinyin, "testpin");
        Assert.AreEqual(2, pinned.Entries.Count);
        Assert.AreEqual(rev2, pinned.Revision);
        Assert.AreEqual("置顶条目甲", pinned.Entries[0].Text);
        Assert.AreEqual("置顶条目乙", pinned.Entries[1].Text);

        // ReorderPinned with matching revision
        ulong rev3 = await client.ReorderPinnedAsync(InputSchemes.Pinyin, "testpin", [p2, p1], expectedRevision: rev2);
        Assert.AreEqual(rev2 + 1, rev3);

        // ReorderPinned with empty list (unpin all items)
        ulong revEmpty = await client.ReorderPinnedAsync(InputSchemes.Pinyin, "testpin", [], expectedRevision: rev3);
        Assert.AreEqual(rev3 + 1, revEmpty);
        var pinnedEmpty = await client.GetPinnedAsync(InputSchemes.Pinyin, "testpin");
        Assert.AreEqual(0, pinnedEmpty.Entries.Count);

        // Re-pin for subsequent tests
        ulong rev4 = await client.SetPinAsync(InputSchemes.Pinyin, "testpin", p1, true, 1, expectedRevision: revEmpty);

        // Scope validation: P1-4 - Pinning an entry into an unrelated scope must be rejected!
        var scopeMismatchEx = await Assert.ThrowsAsync<AdminException>(async () =>
        {
            // p1 has code 'testpin' under Pinyin, not under Wubi86 'aaaa'
            await client.ReorderPinnedAsync(InputSchemes.Wubi86, "aaaa", [p1]);
        });
        Assert.AreEqual(AdminErrorCode.ValidationFailed, scopeMismatchEx.ErrorCode);

        // Shuangpin Alias Canonicalization & Scope Candidates: P1-2
        var spEntryId = await client.AddEntryAsync("双拼测试词", [
            new EntryCodeItem { Scheme = InputSchemes.Pinyin, Code = "zhongwen" }
        ]);
        // Pin using alias "ziranma"
        ulong spRev1 = await client.SetPinAsync(InputSchemes.Shuangpin, "vswf", spEntryId, true, 1, layoutId: "ziranma");
        // Query using canonical "natural"
        var spCanonical = await client.GetPinnedAsync(InputSchemes.Shuangpin, "vswf", layoutId: "natural");
        Assert.AreEqual(1, spCanonical.Entries.Count, "Alias 'ziranma' must canonicalize to 'natural', avoiding scope split");
        Assert.AreEqual("双拼测试词", spCanonical.Entries[0].Text);

        // Query scoped candidates via GetScopeCandidates
        var candidates = await client.GetScopeCandidatesAsync(InputSchemes.Shuangpin, "vswf", layoutId: "natural");
        Assert.IsTrue(candidates.Any(c => c.Text == "双拼测试词"), "GetScopeCandidates must return candidates matching decoded shuangpin branch");

        // ReorderPinned with stale revision -> RevisionConflict (4001)
        var conflictEx = await Assert.ThrowsAsync<AdminException>(async () =>
        {
            await client.ReorderPinnedAsync(InputSchemes.Pinyin, "testpin", [p1, p2], expectedRevision: rev1); // stale!
        });
        Assert.AreEqual(AdminErrorCode.RevisionConflict, conflictEx.ErrorCode);

        // 4. Settings: GetPolicy, SavePolicy with Concurrency Revision, Method Order, Dicts, MixedInput
        var policy = await client.GetPolicyAsync();
        uint origRev = policy.Revision;

        policy.CandidatePageSize = 8;
        policy.LearningEnabled = false;
        policy.MixedInputEnabled = true;
        policy.PrimaryInputMethod = InputSchemes.Pinyin;
        policy.MethodPriorities = [InputSchemes.Pinyin, InputSchemes.Wubi86, InputSchemes.Shuangpin];
        policy.DictionaryPriorities = [
            new DictionaryPriorityItem { DictId = "dict_custom", IsEnabled = false },
            new DictionaryPriorityItem { DictId = "dict_builtin_wubi", IsEnabled = true },
            new DictionaryPriorityItem { DictId = "dict_builtin_pinyin", IsEnabled = true }
        ];
        policy.InitialMode.ChinesePunctuation = false;

        uint newRev = await client.SavePolicyAsync(policy, origRev);
        Assert.AreEqual(origRev + 1, newRev);

        var policySaved = await client.GetPolicyAsync();
        Assert.AreEqual(8, policySaved.CandidatePageSize);
        Assert.AreEqual(false, policySaved.LearningEnabled);
        Assert.AreEqual(true, policySaved.MixedInputEnabled);
        Assert.AreEqual(InputSchemes.Pinyin, policySaved.PrimaryInputMethod);
        Assert.AreEqual(false, policySaved.InitialMode.ChinesePunctuation);
        Assert.AreEqual(3, policySaved.MethodPriorities.Count);
        Assert.AreEqual(InputSchemes.Pinyin, policySaved.MethodPriorities[0]);
        Assert.AreEqual(InputSchemes.Wubi86, policySaved.MethodPriorities[1]);
        Assert.AreEqual(InputSchemes.Shuangpin, policySaved.MethodPriorities[2]);
        Assert.AreEqual("dict_custom", policySaved.DictionaryPriorities[0].DictId);
        Assert.AreEqual(false, policySaved.DictionaryPriorities[0].IsEnabled);

        // Raw JSON inspection to guarantee backend JSON contract
        var rawPolicyDoc = await client.SendRequestAsync("GetPolicy");
        var rawPolicyObj = rawPolicyDoc.GetProperty("policy");
        Assert.AreEqual("[\"pinyin\",\"wubi86\",\"shuangpin\"]", rawPolicyObj.GetProperty("input_method_order").GetRawText());
        Assert.IsTrue(rawPolicyObj.TryGetProperty("enabled_input_methods", out _));
        Assert.IsTrue(rawPolicyObj.TryGetProperty("input_methods", out _));
        Assert.AreEqual(false, rawPolicyObj.GetProperty("initial_mode").GetProperty("chinese_punctuation").GetBoolean());
        Assert.AreEqual(true, rawPolicyObj.GetProperty("mixed_input_enabled").GetBoolean());

        // Test independent input method disabling/enabling via real service
        policy.EnabledInputMethods = [InputSchemes.Pinyin, InputSchemes.Wubi86];
        policy.InputMethods = [
            new InputMethodConfigItem { Scheme = InputSchemes.Pinyin, Enabled = true },
            new InputMethodConfigItem { Scheme = InputSchemes.Wubi86, Enabled = true },
            new InputMethodConfigItem { Scheme = InputSchemes.Shuangpin, Enabled = false }
        ];
        uint spDisabledRev = await client.SavePolicyAsync(policy, newRev);
        Assert.AreEqual(newRev + 1, spDisabledRev);

        var verifySpDisabled = await client.GetPolicyAsync();
        Assert.IsNotNull(verifySpDisabled.EnabledInputMethods);
        CollectionAssert.DoesNotContain(verifySpDisabled.EnabledInputMethods, InputSchemes.Shuangpin);

        // Re-enable Shuangpin to leave policy clean
        policy.EnabledInputMethods = [InputSchemes.Pinyin, InputSchemes.Wubi86, InputSchemes.Shuangpin];
        policy.InputMethods = [
            new InputMethodConfigItem { Scheme = InputSchemes.Pinyin, Enabled = true },
            new InputMethodConfigItem { Scheme = InputSchemes.Wubi86, Enabled = true },
            new InputMethodConfigItem { Scheme = InputSchemes.Shuangpin, Enabled = true }
        ];
        uint reenabledRev = await client.SavePolicyAsync(policy, spDisabledRev);
        Assert.AreEqual(spDisabledRev + 1, reenabledRev);

        // SavePolicy with stale revision -> RevisionConflict (4001)
        var policyConflictEx = await Assert.ThrowsAsync<AdminException>(async () =>
        {
            await client.SavePolicyAsync(policy, origRev); // stale!
        });
        Assert.AreEqual(AdminErrorCode.RevisionConflict, policyConflictEx.ErrorCode);

        // 5. Maintenance: ExportData with version 1.0 format
        await client.ExportDataAsync(s_exportFilePath);
        Assert.IsTrue(File.Exists(s_exportFilePath));

        string exportedContent = await File.ReadAllTextAsync(s_exportFilePath);
        using var jsonDoc = JsonDocument.Parse(exportedContent);
        Assert.IsTrue(jsonDoc.RootElement.TryGetProperty("version", out var verProp));
        Assert.AreEqual("1.0", verProp.GetString());
        Assert.IsTrue(jsonDoc.RootElement.TryGetProperty("entries", out _));

        // Reset stats
        await client.ResetStatsAsync();

        // Delete test entry
        await client.DeleteEntryAsync(entryId);
        var entriesAfterDel = await client.GetEntriesAsync("十三朝古都西安", null, 0, 10);
        Assert.AreEqual(0, entriesAfterDel.Total);
    }

    [TestMethod]
    public async Task TestBuiltinEntryPinningAndLifecycleAcrossSchemes()
    {
        string repoRoot = FindRepoRoot();
        string serviceExe = Path.Combine(repoRoot, @"build\Release\ime-service.exe");
        if (!File.Exists(serviceExe))
        {
            Assert.Inconclusive("ime-service.exe not found at " + serviceExe);
            return;
        }

        string testPipe = $"PersonalIME_Admin_Builtin_{Environment.ProcessId}_{Guid.NewGuid():N}";
        string testDb = Path.Combine(repoRoot, $"out/builtin_e2e_{Environment.ProcessId}_{Guid.NewGuid():N}.db");
        string dictDir = Path.Combine(repoRoot, @"data\dictionaries");
        Directory.CreateDirectory(Path.Combine(repoRoot, "out"));
        if (File.Exists(testDb)) File.Delete(testDb);

        ProcessStartInfo CreatePsi() => new()
        {
            FileName = serviceExe,
            Arguments = $"--db \"{testDb}\" --admin-pipe \"{testPipe}\" --dict \"{dictDir}\"",
            WorkingDirectory = repoRoot,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        var proc = Process.Start(CreatePsi());
        Assert.IsNotNull(proc, "Failed to start isolated service for builtin lifecycle test");

        try
        {
            await Task.Delay(800);
            var client = new AdminIpcClient(testPipe);

            // 1. Fresh Builtin Pin under Wubi86 (Empty DB, not yet materialized into entries table)
            var wbCands = await client.GetScopeCandidatesAsync(InputSchemes.Wubi86, "gh");
            var wbBuiltin = wbCands.FirstOrDefault(c => c.EntryId.StartsWith("b_"));
            Assert.IsNotNull(wbBuiltin, "Must find builtin candidate for wubi86 'gh'");
            Assert.IsFalse(string.IsNullOrEmpty(wbBuiltin.Text), "Builtin candidate text must not be empty");

            // Fresh pin
            ulong wbRev1 = await client.SetPinAsync(InputSchemes.Wubi86, "gh", wbBuiltin.EntryId, true, 1);
            Assert.IsTrue(wbRev1 >= 1);

            var wbPinned = await client.GetPinnedAsync(InputSchemes.Wubi86, "gh");
            Assert.AreEqual(1, wbPinned.Entries.Count);
            Assert.AreEqual(wbBuiltin.EntryId, wbPinned.Entries[0].EntryId);
            Assert.AreEqual("下", wbPinned.Entries[0].Text, "Pinned builtin entry text must resolve to actual text ('下')");
            Assert.AreEqual(1, wbPinned.Entries[0].OrderRank);

            // Reorder Builtin
            ulong wbRev2 = await client.ReorderPinnedAsync(InputSchemes.Wubi86, "gh", [wbBuiltin.EntryId], expectedRevision: wbRev1);
            Assert.AreEqual(wbRev1 + 1, wbRev2);

            // Cross-scope rejection for builtin entry
            var exScope = await Assert.ThrowsAsync<AdminException>(async () =>
            {
                await client.SetPinAsync(InputSchemes.Wubi86, "aaaa", wbBuiltin.EntryId, true, 1);
            });
            Assert.AreEqual(AdminErrorCode.ValidationFailed, exScope.ErrorCode);

            var exScheme = await Assert.ThrowsAsync<AdminException>(async () =>
            {
                await client.ReorderPinnedAsync(InputSchemes.Pinyin, "xian", [wbBuiltin.EntryId]);
            });
            Assert.AreEqual(AdminErrorCode.ValidationFailed, exScheme.ErrorCode);

            // 2. Materialized Builtin: restore/import merge then re-pin and re-order
            string importPath = Path.Combine(repoRoot, $"out/builtin_merge_{Environment.ProcessId}_{Guid.NewGuid():N}.json");
            await File.WriteAllTextAsync(importPath, JsonSerializer.Serialize(new
            {
                version = "1.0",
                entries = Array.Empty<object>(),
                stats = Array.Empty<object>(),
                preferences = new[]
                {
                    new { scheme = InputSchemes.Wubi86, code = "gh", entry_id = wbBuiltin.EntryId, is_pinned = true, rank = 1 }
                }
            }));
            await client.ImportDataAsync(importPath, "merge");
            if (File.Exists(importPath)) File.Delete(importPath);

            // Re-pin & reorder materialized builtin (must NOT fail with "Entry does not belong to target query scope")
            ulong wbRev3 = await client.SetPinAsync(InputSchemes.Wubi86, "gh", wbBuiltin.EntryId, true, 1);
            Assert.IsTrue(wbRev3 >= 1);
            ulong wbRev4 = await client.ReorderPinnedAsync(InputSchemes.Wubi86, "gh", [wbBuiltin.EntryId]);
            Assert.IsTrue(wbRev4 >= 1);

            // 3. Pinyin & Shuangpin Builtin Pinning
            var pyCands = await client.GetScopeCandidatesAsync(InputSchemes.Pinyin, "zhong");
            var pyBuiltin = pyCands.FirstOrDefault(c => c.EntryId.StartsWith("b_") && c.Text == "中")
                         ?? pyCands.First(c => c.EntryId.StartsWith("b_"));
            Assert.IsNotNull(pyBuiltin, "Must find builtin candidate for pinyin 'zhong'");
            string expectedPyText = pyBuiltin.Text;

            ulong pyRev = await client.SetPinAsync(InputSchemes.Pinyin, "zhong", pyBuiltin.EntryId, true, 1);
            Assert.IsTrue(pyRev >= 1);
            var pyPinned = await client.GetPinnedAsync(InputSchemes.Pinyin, "zhong");
            Assert.AreEqual(1, pyPinned.Entries.Count);
            Assert.AreEqual(expectedPyText, pyPinned.Entries[0].Text);

            // Shuangpin under layout "natural" code "vs" (decodes to zh-ong)
            ulong spRev = await client.SetPinAsync(InputSchemes.Shuangpin, "vs", pyBuiltin.EntryId, true, 1, layoutId: "natural");
            Assert.IsTrue(spRev >= 1);
            var spPinned = await client.GetPinnedAsync(InputSchemes.Shuangpin, "vs", layoutId: "natural");
            Assert.AreEqual(1, spPinned.Entries.Count);
            Assert.AreEqual(expectedPyText, spPinned.Entries[0].Text);

            // 4. Service Restart Persistence Verification
            proc.Kill();
            await proc.WaitForExitAsync();
            proc.Dispose();

            proc = Process.Start(CreatePsi());
            Assert.IsNotNull(proc);
            await Task.Delay(800);

            // Reconnect client to restarted service and verify all pinned builtin entries survived with texts
            var restartClient = new AdminIpcClient(testPipe);
            var wbRestart = await restartClient.GetPinnedAsync(InputSchemes.Wubi86, "gh");
            Assert.AreEqual(1, wbRestart.Entries.Count, "Wubi pinned builtin entry must persist across service restart");
            Assert.AreEqual("下", wbRestart.Entries[0].Text, "Wubi pinned builtin entry text must not be empty after restart");

            var pyRestart = await restartClient.GetPinnedAsync(InputSchemes.Pinyin, "zhong");
            Assert.AreEqual(1, pyRestart.Entries.Count, "Pinyin pinned builtin entry must persist across service restart");
            Assert.AreEqual(expectedPyText, pyRestart.Entries[0].Text, "Pinyin pinned builtin entry text must not be empty after restart");

            var spRestart = await restartClient.GetPinnedAsync(InputSchemes.Shuangpin, "vs", layoutId: "natural");
            Assert.AreEqual(1, spRestart.Entries.Count, "Shuangpin pinned builtin entry must persist across service restart");
            Assert.AreEqual(expectedPyText, spRestart.Entries[0].Text, "Shuangpin pinned builtin entry text must not be empty after restart");

            // 5. Unpin builtin
            await restartClient.SetPinAsync(InputSchemes.Wubi86, "gh", wbBuiltin.EntryId, false, 1);
            var wbUnpinned = await restartClient.GetPinnedAsync(InputSchemes.Wubi86, "gh");
            Assert.AreEqual(0, wbUnpinned.Entries.Count, "Pinned list must be empty after unpinning");
        }
        finally
        {
            if (proc != null && !proc.HasExited)
            {
                proc.Kill();
                await proc.WaitForExitAsync();
                proc.Dispose();
            }
            try
            {
                if (File.Exists(testDb)) File.Delete(testDb);
            }
            catch { }
        }
    }

    [TestMethod]
    public async Task TestManagerAutoLaunchAndRecoveryLifecycle()
    {
        // 1. Verify LocateServiceExecutable returns a valid path
        string? servicePath = MainViewModel.LocateServiceExecutable();
        Assert.IsNotNull(servicePath, "LocateServiceExecutable must find a valid ime-service.exe");
        Assert.IsTrue(File.Exists(servicePath), $"Service path does not exist: {servicePath}");

        // 2. Connect to the test service running in this test class
        var client = new AdminIpcClient(s_testPipeName);
        using var vm = new MainViewModel(client);
        await vm.CheckConnectionAsync();

        Assert.IsTrue(vm.IsConnected);
        Assert.IsFalse(vm.HasOfflineWarning);
        Assert.AreEqual(ServiceConnectionState.Connected, vm.ServiceState);
        Assert.IsNotNull(vm.ActiveSchemeText);
        Assert.IsNotNull(vm.ActiveMixedInputText);
        Assert.IsNotNull(vm.ActiveInitialModeText);

        // 3. Verify saving in Settings updates Active display
        vm.Settings.PrimaryInputMethod = InputSchemes.Wubi86;
        vm.Settings.MixedInputEnabled = true;
        vm.Settings.InitialChinese = true;
        await vm.Settings.SavePolicyAsync();

        Assert.AreEqual("五笔86", vm.ActiveSchemeText);
        Assert.AreEqual("开启", vm.ActiveMixedInputText);
        Assert.AreEqual("中文", vm.ActiveInitialModeText);
        StringAssert.Contains(vm.ConfigSavedStatusText, "已保存生效");
    }
}
