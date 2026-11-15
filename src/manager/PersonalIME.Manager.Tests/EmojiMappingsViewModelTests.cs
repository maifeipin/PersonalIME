using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class EmojiMappingsViewModelTests
{
    private sealed class InterceptingMockAdminClient : MockAdminClient
    {
        private int _inFlightSaves;

        public int MaxConcurrentSaves { get; private set; }
        public List<bool> SaveCalls { get; } = [];
        public Func<bool, Task>? SaveOverride { get; set; }
        public Func<Task<EmojiMappingsResult>>? GetOverride { get; set; }

        public override Task<EmojiMappingsResult> GetEmojiMappingsAsync(bool onlyEnabled = false, CancellationToken ct = default)
        {
            return GetOverride != null ? GetOverride() : base.GetEmojiMappingsAsync(onlyEnabled, ct);
        }

        public override async Task SetEmojiAssociationEnabledAsync(bool enabled, CancellationToken ct = default)
        {
            int concurrent = Interlocked.Increment(ref _inFlightSaves);
            MaxConcurrentSaves = Math.Max(MaxConcurrentSaves, concurrent);
            SaveCalls.Add(enabled);
            try
            {
                if (SaveOverride != null)
                {
                    await SaveOverride(enabled);
                }
                else
                {
                    await base.SetEmojiAssociationEnabledAsync(enabled, ct);
                }
            }
            finally
            {
                Interlocked.Decrement(ref _inFlightSaves);
            }
        }
    }

    [TestMethod]
    public async Task TestAssociationToggleWhenDisconnectedRevertsToConfirmedValue()
    {
        var mock = new InterceptingMockAdminClient { MockEmojiAssociationEnabled = true };
        var banner = new NotificationBanner();
        var vm = new EmojiMappingsViewModel(mock, banner);

        await vm.LoadDataAsync();
        Assert.IsTrue(vm.IsAssociationEnabled);

        mock.IsConnected = false;
        vm.IsAssociationEnabled = false;
        await vm.PendingAssociationSaveTask;

        Assert.IsTrue(vm.IsAssociationEnabled);
        Assert.IsTrue(mock.MockEmojiAssociationEnabled);
        Assert.AreEqual(0, mock.SaveCalls.Count);
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, banner.Type);
    }

    [TestMethod]
    public async Task TestAssociationToggleServerRejectionRevertsToServerState()
    {
        var mock = new InterceptingMockAdminClient
        {
            MockEmojiAssociationEnabled = true,
            SaveOverride = _ => throw new InvalidOperationException("IPC rejected")
        };
        var banner = new NotificationBanner();
        var vm = new EmojiMappingsViewModel(mock, banner);

        await vm.LoadDataAsync();
        Assert.IsTrue(vm.IsAssociationEnabled);

        vm.IsAssociationEnabled = false;
        await vm.PendingAssociationSaveTask;

        Assert.IsTrue(vm.IsAssociationEnabled);
        Assert.IsTrue(mock.MockEmojiAssociationEnabled);
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Error, banner.Type);
    }

    [TestMethod]
    public async Task TestRapidConsecutiveAssociationTogglesSerializeWithoutConcurrentRaces()
    {
        var firstCallEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var allowFirstCallFinish = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int callCount = 0;

        var mock = new InterceptingMockAdminClient { MockEmojiAssociationEnabled = true };
        mock.SaveOverride = async enabled =>
        {
            int idx = Interlocked.Increment(ref callCount);
            if (idx == 1)
            {
                firstCallEntered.TrySetResult();
                await allowFirstCallFinish.Task;
            }
            mock.MockEmojiAssociationEnabled = enabled;
        };

        var banner = new NotificationBanner();
        var vm = new EmojiMappingsViewModel(mock, banner);
        await vm.LoadDataAsync();

        vm.IsAssociationEnabled = false;
        await firstCallEntered.Task;

        // Rapidly toggle while first IPC save is still in flight
        vm.IsAssociationEnabled = true;
        vm.IsAssociationEnabled = false;
        vm.IsAssociationEnabled = true;

        allowFirstCallFinish.TrySetResult();
        await vm.PendingAssociationSaveTask;

        Assert.AreEqual(1, mock.MaxConcurrentSaves);
        Assert.IsTrue(vm.IsAssociationEnabled);
        Assert.IsTrue(mock.MockEmojiAssociationEnabled);
        Assert.AreEqual(true, mock.SaveCalls[^1]);
        Assert.IsFalse(vm.IsSavingAssociationEnabled);
    }
}
