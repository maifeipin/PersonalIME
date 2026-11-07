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
}
