using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class MainViewModelTests
{
    [TestMethod]
    public async Task TestInitialOfflineStateAndReadinessProperties()
    {
        var mock = new MockAdminClient
        {
            OnGetStatus = _ => throw new TimeoutException("Connection refused")
        };
        using var vm = new MainViewModel(mock);
        await vm.CheckConnectionAsync();

        Assert.IsFalse(vm.IsConnected);
        Assert.IsTrue(vm.HasOfflineWarning);
        StringAssert.Contains(vm.OfflineWarningText, "后台服务离线");
        StringAssert.Contains(vm.OfflineWarningText, "英文直通");
    }

    [TestMethod]
    public async Task TestConnectedStatePopulatesEffectivePolicy()
    {
        var mock = new MockAdminClient
        {
            OnGetPolicy = _ => Task.FromResult(new InputPolicyConfig
            {
                Revision = 3,
                PrimaryInputMethod = InputSchemes.Wubi86,
                MixedInputEnabled = true,
                InitialMode = new InitialModeConfig { Chinese = true }
            })
        };

        using var vm = new MainViewModel(mock);
        await vm.CheckConnectionAsync();

        Assert.IsTrue(vm.IsConnected);
        Assert.IsFalse(vm.HasOfflineWarning);
        Assert.AreEqual(ServiceConnectionState.Connected, vm.ServiceState);
        Assert.AreEqual("五笔86", vm.ActiveSchemeText);
        Assert.AreEqual("开启", vm.ActiveMixedInputText);
        Assert.AreEqual("中文", vm.ActiveInitialModeText);
        StringAssert.Contains(vm.ConfigSavedStatusText, "rev.3");
    }

    [TestMethod]
    public async Task TestPolicySavedSyncsToMainViewModelActiveDisplay()
    {
        var mock = new MockAdminClient();
        using var vm = new MainViewModel(mock);
        await vm.CheckConnectionAsync();

        // Simulate save policy in Settings
        vm.Settings.PrimaryInputMethod = InputSchemes.Pinyin;
        vm.Settings.MixedInputEnabled = false;
        vm.Settings.InitialChinese = false;
        await vm.Settings.SavePolicyAsync();

        Assert.AreEqual("全拼", vm.ActiveSchemeText);
        Assert.AreEqual("关闭", vm.ActiveMixedInputText);
        Assert.AreEqual("英文", vm.ActiveInitialModeText);
    }

    [TestMethod]
    public void TestLocateServiceExecutable()
    {
        // Must not throw exception
        string? path = MainViewModel.LocateServiceExecutable();
        // If file exists, it must end with ime-service.exe
        if (path != null)
        {
            StringAssert.EndsWith(path.ToLowerInvariant(), "ime-service.exe");
        }
    }

    [TestMethod]
    public async Task TestConnectionActionButtonTextDynamicChanges()
    {
        var mock = new MockAdminClient
        {
            OnGetStatus = _ => throw new TimeoutException("Connection refused")
        };
        using var vm = new MainViewModel(mock);
        await vm.CheckConnectionAsync();

        Assert.IsFalse(vm.IsConnected);
        Assert.AreEqual("▶ 启动服务", vm.ConnectionActionButtonText);

        mock.OnGetStatus = _ => Task.FromResult(new ServiceStatusInfo { ServiceVersion = "1.0.0" });
        await vm.CheckConnectionAsync();

        Assert.IsTrue(vm.IsConnected);
        Assert.AreEqual("刷新连接", vm.ConnectionActionButtonText);
    }

    [TestMethod]
    public async Task TestMaintenanceRestartServiceCommandDelegation()
    {
        bool restartCalled = false;
        var banner = new NotificationBanner();
        var mock = new MockAdminClient();
        var vm = new MaintenanceViewModel(mock, banner, () =>
        {
            restartCalled = true;
            return Task.CompletedTask;
        });

        Assert.IsNotNull(vm.RestartServiceCommand);
        await vm.RestartServiceCommand.ExecuteAsync(null);
        Assert.IsTrue(restartCalled);
    }
}
