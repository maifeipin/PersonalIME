using System.Text.Json;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class PluginsViewModelTests
{
    [TestMethod]
    public async Task LoadDataAsync_PopulatesPluginsAndLogs()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new PluginsViewModel(client, banner);

        await vm.LoadDataAsync();

        Assert.AreEqual(3, vm.Plugins.Count);
        Assert.AreEqual("计算器", vm.Plugins[0].Name);
        Assert.AreEqual("画图", vm.Plugins[1].Name);
        Assert.AreEqual("Edge浏览器", vm.Plugins[2].Name);
        Assert.IsNotNull(vm.SelectedPlugin);
    }

    [TestMethod]
    public async Task SwitchingToPluginsTab_Index3_LoadsPluginsFromDatabase()
    {
        var client = new MockAdminClient();
        using var mainVm = new MainViewModel(client);
        await mainVm.InitializeAsync();

        mainVm.SelectedTabIndex = 3; // Tab 3 = 插件管理
        await Task.Delay(50);

        Assert.AreEqual(3, mainVm.Plugins.Plugins.Count);
        Assert.AreEqual("计算器", mainVm.Plugins.Plugins[0].Name);
    }

    [TestMethod]
    public void Deserialize_NumericUnixTimestampsAndMessage_Succeeds()
    {
        string pluginsJson = """
        [
            {"id":1,"name":"计算器","icon":"🧮","exec_path":"calc.exe","arguments":"","is_enabled":true,"sort_order":0,"created_at":1730956800}
        ]
        """;
        var plugins = JsonSerializer.Deserialize<List<PluginModel>>(pluginsJson);
        Assert.IsNotNull(plugins);
        Assert.AreEqual(1, plugins.Count);
        Assert.AreEqual("计算器", plugins[0].Name);
        Assert.IsFalse(string.IsNullOrEmpty(plugins[0].CreatedAt));

        string logsJson = """
        [
            {"id":10,"plugin_id":1,"plugin_name":"计算器","command_line":"calc.exe","executed_at":1730956800,"status":"SUCCESS","message":"Process launched successfully"}
        ]
        """;
        var logs = JsonSerializer.Deserialize<List<PluginLogModel>>(logsJson);
        Assert.IsNotNull(logs);
        Assert.AreEqual(1, logs.Count);
        Assert.AreEqual("Process launched successfully", logs[0].ErrorMessage);
        Assert.IsFalse(string.IsNullOrEmpty(logs[0].ExecutedAt));
    }

    [TestMethod]
    public async Task AddPlugin_IncreasesCountAndSelects()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new PluginsViewModel(client, banner);
        await vm.LoadDataAsync();

        vm.PrepareNewPlugin();
        vm.FormName = "截图工具";
        vm.FormIcon = "✂️";
        vm.FormExecPath = "snippingtool.exe";
        vm.FormArguments = "/clip";

        await vm.SaveFormAsync();

        Assert.AreEqual(4, vm.Plugins.Count);
        Assert.IsTrue(vm.Plugins.Any(p => p.Name == "截图工具"));
    }

    [TestMethod]
    public async Task DeletePlugin_RemovesFromCollection()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new PluginsViewModel(client, banner);
        await vm.LoadDataAsync();

        vm.SelectedPlugin = vm.Plugins[1]; // 画图
        await vm.DeletePluginAsync();

        Assert.AreEqual(2, vm.Plugins.Count);
        Assert.IsFalse(vm.Plugins.Any(p => p.Name == "画图"));
    }

    [TestMethod]
    public async Task ExecutePlugin_CreatesLogRecord()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new PluginsViewModel(client, banner);
        await vm.LoadDataAsync();

        await vm.ExecutePluginAsync(vm.Plugins[0]);

        Assert.AreEqual(1, client.ExecutePluginCallCount);
        Assert.AreEqual(1, vm.Logs.Count);
        Assert.AreEqual("SUCCESS", vm.Logs[0].Status);
        Assert.AreEqual("计算器", vm.Logs[0].PluginName);
    }

    [TestMethod]
    public async Task ClearLogs_EmptiesLogsCollection()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new PluginsViewModel(client, banner);
        await vm.LoadDataAsync();

        await vm.ExecutePluginAsync(vm.Plugins[0]);
        Assert.AreEqual(1, vm.Logs.Count);

        await vm.ClearLogsAsync();
        Assert.AreEqual(0, vm.Logs.Count);
    }

    [TestMethod]
    public async Task WordbookSave_AutoCommitsPendingCode_PreservesFailedDraft_AndAvoidsDuplicates()
    {
        var client = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new WordbookViewModel(client, banner);

        // 1. Save fails when pending NewCodeText is invalid -> NewCodeText preserved, EditorCodes untouched
        vm.OpenAddDialogCommand.Execute(null);
        vm.EditorText = "人人输入法";
        vm.NewCodeScheme = InputSchemes.Wubi86;
        vm.NewCodeText = "rrsyz"; // invalid 'z'
        client.OnAddEntry = (_, _, _) => throw new PersonalIME.Manager.Ipc.AdminException(
            PersonalIME.Manager.Ipc.AdminErrorCode.ValidationFailed, "编码格式不合法");

        await vm.SaveEditorAsync();

        Assert.IsTrue(vm.IsEditorOpen);
        Assert.AreEqual("rrsyz", vm.NewCodeText, "Failed save must preserve draft in NewCodeText");
        Assert.AreEqual(0, vm.EditorCodes.Count, "Failed save must not pollute EditorCodes");

        // 2. Fix NewCodeText to 'rrsyf' and also test duplicate avoidance if already in EditorCodes
        vm.NewCodeText = "rrsyf";
        vm.AddCodeToDraftCommand.Execute(null);
        Assert.AreEqual(1, vm.EditorCodes.Count);
        vm.NewCodeText = "RRSYF"; // duplicate (case-insensitive)

        List<EntryCodeItem>? sentCodes = null;
        client.OnAddEntry = (_, codes, _) =>
        {
            sentCodes = codes.ToList();
            return Task.FromResult("u_rrsyf_1");
        };

        await vm.SaveEditorAsync();

        Assert.IsFalse(vm.IsEditorOpen);
        Assert.IsNotNull(sentCodes);
        Assert.AreEqual(1, sentCodes.Count, "Must not send duplicate code when NewCodeText matches EditorCodes");
        Assert.AreEqual("rrsyf", sentCodes[0].Code);
        Assert.AreEqual(string.Empty, vm.NewCodeText);
    }
}
