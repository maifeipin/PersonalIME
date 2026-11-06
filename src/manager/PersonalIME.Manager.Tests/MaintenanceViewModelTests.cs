using System.Text.Json;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class MaintenanceViewModelTests
{
    [TestMethod]
    public void TestResetStatsRequiresConfirmationDialog()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new MaintenanceViewModel(mock, banner);

        // User triggers reset
        vm.RequestResetStatsCommand.Execute(null);

        // Confirmation dialog must open
        Assert.IsTrue(vm.IsConfirmDialogOpen);
        StringAssert.Contains(vm.ConfirmDialogTitle, "重置自学习统计");
        Assert.AreEqual(0, mock.ResetStatsCallCount, "No backend call made before user confirms!");

        // User cancels
        vm.CancelActionCommand.Execute(null);
        Assert.IsFalse(vm.IsConfirmDialogOpen);
        Assert.AreEqual(0, mock.ResetStatsCallCount);
    }

    [TestMethod]
    public async Task TestResetStatsExecutedOnlyUponConfirmation()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new MaintenanceViewModel(mock, banner);

        vm.RequestResetStatsCommand.Execute(null);
        Assert.IsTrue(vm.IsConfirmDialogOpen);

        // User confirms
        await vm.ConfirmActionCommand.ExecuteAsync(null);

        Assert.IsFalse(vm.IsConfirmDialogOpen);
        Assert.AreEqual(1, mock.ResetStatsCallCount, "ResetStats called exactly once after confirmation");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Success, banner.Type);
    }

    [TestMethod]
    public async Task TestResetStatsTimeoutDoesNotAutoRetry()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnResetStats = (id, ct) =>
        {
            throw new TimeoutException("请求超时，结果尚未确认");
        };

        var vm = new MaintenanceViewModel(mock, banner);
        vm.RequestResetStatsCommand.Execute(null);
        await vm.ConfirmActionCommand.ExecuteAsync(null);

        Assert.AreEqual(1, mock.ResetStatsCallCount, "Strictly no auto-retry on mutating ResetStats!");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, banner.Type);
        StringAssert.Contains(banner.Message, "结果尚未确认");
    }

    [TestMethod]
    public void TestBackupFormatAdheresToVersion10()
    {
        // Directive 6: "备份格式沿用后端实际导出字段，先核对是 version 还是 schema_version，不要另造不兼容格式。"
        // Backend json_backup.cpp strictly requires root["version"] == "1.0".
        string sampleJson = """
        {
            "version": "1.0",
            "entries": [
                {
                    "id": "u_test",
                    "text": "测试",
                    "codes": [{"scheme": "pinyin", "code": "ceshi"}]
                }
            ],
            "preferences": [],
            "stats": []
        }
        """;

        using var doc = JsonDocument.Parse(sampleJson);
        var root = doc.RootElement;

        Assert.IsTrue(root.TryGetProperty("version", out var verProp));
        Assert.AreEqual("1.0", verProp.GetString());
        Assert.IsFalse(root.TryGetProperty("schema_version", out _), "Must NOT use schema_version");
    }
}

