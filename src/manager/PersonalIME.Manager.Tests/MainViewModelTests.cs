using PersonalIME.Manager.Ipc;
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

    [TestMethod]
    public async Task TestRestartServiceCustomPipeRejection()
    {
        var customClient = new AdminIpcClient("PersonalIME_Custom_Isolated_Pipe_12345");
        using var vm = new MainViewModel(customClient);

        await vm.RestartServiceAsync();

        Assert.IsTrue(vm.Banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, vm.Banner.Type);
        StringAssert.Contains(vm.Banner.Message, "自定义管道");
        StringAssert.Contains(vm.Banner.Message, "已拒绝重启");
    }

    [TestMethod]
    public async Task TestRestartServiceUnverifiedProcessIdRejection()
    {
        var mock = new MockAdminClient
        {
            OnGetStatus = _ => Task.FromResult(new ServiceStatusInfo
            {
                ServiceVersion = "1.0.0",
                ProcessId = 0 // Unverified PID
            })
        };
        using var vm = new MainViewModel(mock);
        await vm.CheckConnectionAsync();
        Assert.IsTrue(vm.IsConnected);

        await vm.RestartServiceAsync();

        Assert.IsTrue(vm.Banner.IsVisible);
        Assert.AreEqual(BannerType.Error, vm.Banner.Type);
        StringAssert.Contains(vm.Banner.Message, "无法获取当前连接服务的实例身份 (PID)");
        Assert.IsTrue(vm.IsConnected, "Must remain connected and safe without blind kill");
        Assert.IsTrue(vm.IsConnected, "Must remain connected and safe without blind kill");
    }

    [TestMethod]
    public async Task TestLifecycleLockPreventsConcurrentReentrancy()
    {
        var tcs = new TaskCompletionSource<ServiceStatusInfo>();
        var mock = new MockAdminClient
        {
            OnGetStatus = async ct =>
            {
                return await tcs.Task;
            }
        };
        using var vm = new MainViewModel(mock);

        // Start check connection (will hold lock until tcs is set)
        var checkTask = vm.CheckConnectionAsync();

        Assert.IsTrue(vm.IsLifecycleBusy);
        Assert.IsFalse(vm.CanPerformLifecycleAction);
        Assert.IsFalse(vm.CheckConnectionCommand.CanExecute(null));
        Assert.IsFalse(vm.RestartServiceCommand.CanExecute(null));

        // Attempting to restart while busy should immediately return without doing anything
        await vm.RestartServiceAsync();

        // Release the pending operation
        tcs.SetResult(new ServiceStatusInfo { ServiceVersion = "1.0.0" });
        await checkTask;

        Assert.IsFalse(vm.IsLifecycleBusy);
        Assert.IsTrue(vm.CanPerformLifecycleAction);
        Assert.IsTrue(vm.CheckConnectionCommand.CanExecute(null));
        Assert.IsTrue(vm.RestartServiceCommand.CanExecute(null));
    }
}
