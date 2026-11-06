using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class SettingsViewModelTests
{
    [TestMethod]
    public void TestIndependentPriorityReordering()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new SettingsViewModel(mock, banner);

        // Initial method priority: [wubi86, pinyin, shuangpin, custom]
        Assert.AreEqual(InputSchemes.Wubi86, vm.MethodPriorities[0].Scheme);
        Assert.AreEqual(InputSchemes.Pinyin, vm.MethodPriorities[1].Scheme);

        // Initial dict priority: [dict_custom, dict_builtin_wubi, dict_builtin_pinyin]
        Assert.AreEqual("dict_custom", vm.DictionaryPriorities[0].Id);
        Assert.AreEqual("dict_builtin_wubi", vm.DictionaryPriorities[1].Id);

        // 1. Move Pinyin up in Method Priorities
        vm.SelectedMethodPriority = vm.MethodPriorities[1];
        vm.MoveMethodUp();

        Assert.AreEqual(InputSchemes.Pinyin, vm.MethodPriorities[0].Scheme);
        Assert.AreEqual(InputSchemes.Wubi86, vm.MethodPriorities[1].Scheme);

        // Dictionary priorities must remain completely untouched!
        Assert.AreEqual("dict_custom", vm.DictionaryPriorities[0].Id);
        Assert.AreEqual("dict_builtin_wubi", vm.DictionaryPriorities[1].Id);

        // 2. Move dict_builtin_wubi up in Dictionary Priorities
        vm.SelectedDictPriority = vm.DictionaryPriorities[1];
        vm.MoveDictUp();

        Assert.AreEqual("dict_builtin_wubi", vm.DictionaryPriorities[0].Id);
        Assert.AreEqual("dict_custom", vm.DictionaryPriorities[1].Id);

        // Method priorities remain [pinyin, wubi86, ...]
        Assert.AreEqual(InputSchemes.Pinyin, vm.MethodPriorities[0].Scheme);
        Assert.AreEqual(InputSchemes.Wubi86, vm.MethodPriorities[1].Scheme);
    }

    [TestMethod]
    public void TestCandidatePageSizeClampedBetween5And9()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new SettingsViewModel(mock, banner);

        vm.CandidatePageSize = 3;
        Assert.AreEqual(5, vm.CandidatePageSize);

        vm.CandidatePageSize = 10;
        Assert.AreEqual(9, vm.CandidatePageSize);

        vm.CandidatePageSize = 7;
        Assert.AreEqual(7, vm.CandidatePageSize);
    }

    [TestMethod]
    public async Task TestSavePolicyTimeoutPreservesDraftWithoutAutoRetry()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnSavePolicy = (cfg, expRev, ct) =>
        {
            throw new TimeoutException("请求超时，结果尚未确认");
        };

        var vm = new SettingsViewModel(mock, banner)
        {
            CandidatePageSize = 8,
            PrimaryInputMethod = InputSchemes.Shuangpin
        };

        await vm.SavePolicyAsync();

        Assert.AreEqual(1, mock.SavePolicyCallCount, "SavePolicy must not auto-retry on timeout");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, banner.Type);
        StringAssert.Contains(banner.Message, "结果尚未确认");

        // Settings draft is preserved in ViewModel
        Assert.AreEqual(8, vm.CandidatePageSize);
        Assert.AreEqual(InputSchemes.Shuangpin, vm.PrimaryInputMethod);
    }

    [TestMethod]
    public async Task TestSavePolicyConcurrencyConflict()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnSavePolicy = (cfg, expRev, ct) =>
        {
            throw new AdminException(AdminErrorCode.RevisionConflict, "Concurrent update detected");
        };

        var vm = new SettingsViewModel(mock, banner);
        await vm.SavePolicyAsync();

        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Error, banner.Type);
        StringAssert.Contains(banner.Message, "并发版本冲突");
    }

    [TestMethod]
    public async Task TestPrimaryInputMethodCannotBeDisabled()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new SettingsViewModel(mock, banner);

        // Primary is Wubi86 by default
        Assert.AreEqual(InputSchemes.Wubi86, vm.PrimaryInputMethod);

        // User unchecks Wubi86
        var wubiItem = vm.MethodPriorities.First(m => m.Scheme == InputSchemes.Wubi86);
        wubiItem.IsEnabled = false;

        await vm.SavePolicyAsync();

        Assert.AreEqual(0, mock.SavePolicyCallCount, "Must not send save request when primary method is disabled");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Error, banner.Type);
        StringAssert.Contains(banner.Message, "默认（首选）输入方式必须处于启用状态");
    }

    [TestMethod]
    public async Task TestAtLeastOneMethodMustBeEnabled()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new SettingsViewModel(mock, banner);

        foreach (var m in vm.MethodPriorities)
        {
            m.IsEnabled = false;
        }

        await vm.SavePolicyAsync();

        Assert.AreEqual(0, mock.SavePolicyCallCount, "Must not send save request when all methods are disabled");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Error, banner.Type);
        StringAssert.Contains(banner.Message, "至少需要启用一种输入方式");
    }

    [TestMethod]
    public async Task TestInputMethodEnabledStatePreservedInSave()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        InputPolicyConfig? capturedConfig = null;
        mock.OnSavePolicy = (cfg, expRev, ct) =>
        {
            capturedConfig = cfg;
            return Task.FromResult(expRev + 1);
        };

        var vm = new SettingsViewModel(mock, banner);

        // Disable Shuangpin
        var spItem = vm.MethodPriorities.First(m => m.Scheme == InputSchemes.Shuangpin);
        spItem.IsEnabled = false;

        await vm.SavePolicyAsync();

        Assert.AreEqual(1, mock.SavePolicyCallCount);
        Assert.IsNotNull(capturedConfig);
        Assert.IsNotNull(capturedConfig.EnabledInputMethods);
        CollectionAssert.DoesNotContain(capturedConfig.EnabledInputMethods, InputSchemes.Shuangpin);
        CollectionAssert.Contains(capturedConfig.EnabledInputMethods, InputSchemes.Wubi86);
        CollectionAssert.Contains(capturedConfig.EnabledInputMethods, InputSchemes.Pinyin);

        var spConfigItem = capturedConfig.InputMethods?.FirstOrDefault(m => m.Scheme == InputSchemes.Shuangpin);
        Assert.IsNotNull(spConfigItem);
        Assert.IsFalse(spConfigItem.Enabled);
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Success, banner.Type);
    }

    [TestMethod]
    public void TestSwitchingPrimaryMethodAutoEnablesIt()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new SettingsViewModel(mock, banner);

        var spItem = vm.MethodPriorities.First(m => m.Scheme == InputSchemes.Shuangpin);
        spItem.IsEnabled = false;
        Assert.IsFalse(spItem.IsEnabled);

        // Switch primary method to Shuangpin
        vm.PrimaryInputMethod = InputSchemes.Shuangpin;

        // Must automatically re-enable it
        Assert.IsTrue(spItem.IsEnabled);
    }

    [TestMethod]
    public async Task TestReverseLookupLoadAndSaveRoundtrip()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPolicy = (ct) => Task.FromResult(new InputPolicyConfig
        {
            Revision = 10,
            CandidatePageSize = 6,
            ReverseLookup = new ReverseLookupConfig
            {
                Enabled = true,
                TargetMethod = InputSchemes.Wubi86,
                ShuangpinLayout = ShuangpinLayouts.Natural,
                Timing = "always"
            }
        });

        InputPolicyConfig? savedConfig = null;
        mock.OnSavePolicy = (cfg, expRev, ct) =>
        {
            savedConfig = cfg;
            return Task.FromResult(expRev + 1);
        };

        var vm = new SettingsViewModel(mock, banner);
        await vm.LoadPolicyAsync();

        // 1. Verify loaded properties
        Assert.IsTrue(vm.ReverseLookupEnabled);
        Assert.AreEqual(InputSchemes.Wubi86, vm.ReverseLookupTargetMethod);
        Assert.AreEqual(ShuangpinLayouts.Natural, vm.ReverseLookupShuangpinLayout);
        Assert.AreEqual("always", vm.ReverseLookupTiming);
        Assert.IsFalse(vm.IsShuangpinTarget);

        // 2. Mutate properties to Shuangpin
        vm.ReverseLookupTargetMethod = InputSchemes.Shuangpin;
        vm.ReverseLookupShuangpinLayout = ShuangpinLayouts.Xiaohe;
        vm.ReverseLookupTiming = "composition";
        Assert.IsTrue(vm.IsShuangpinTarget);

        // 3. Save
        await vm.SavePolicyAsync();

        Assert.AreEqual(1, mock.SavePolicyCallCount);
        Assert.IsNotNull(savedConfig);
        Assert.IsNotNull(savedConfig.ReverseLookup);
        Assert.IsTrue(savedConfig.ReverseLookup.Enabled);
        Assert.AreEqual(InputSchemes.Shuangpin, savedConfig.ReverseLookup.TargetMethod);
        Assert.AreEqual(ShuangpinLayouts.Xiaohe, savedConfig.ReverseLookup.ShuangpinLayout);
        Assert.AreEqual("composition", savedConfig.ReverseLookup.Timing);
        Assert.AreEqual(BannerType.Success, banner.Type);
    }
}

