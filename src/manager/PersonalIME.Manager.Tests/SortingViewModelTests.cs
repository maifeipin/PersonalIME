using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class SortingViewModelTests
{
    [TestMethod]
    public async Task TestRefreshPendingDiscardsOldSaveBeforeLoadCompletes()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult { Revision = 1 });
        var vm = new SortingViewModel(mock, banner) { QueryCode = "aaaa" };
        await vm.LoadPinnedAsync();

        var saveGate = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        var loadGate = new TaskCompletionSource<GetPinnedResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnReorderPinned = (s, c, ids, l, r, ct) => saveGate.Task;
        var saving = vm.SaveSortOrderAsync();
        mock.OnGetPinned = (s, c, l, ct) => loadGate.Task;
        var refreshing = vm.LoadPinnedAsync();
        banner.ShowInfo("refresh pending");

        saveGate.SetResult(2);
        await saving;
        Assert.AreEqual(1UL, vm.CurrentRevision, "Old save must not update a refreshing draft");
        Assert.IsTrue(vm.IsLoading, "Old save must not clear the refresh busy state");
        Assert.AreEqual("refresh pending", banner.Message);

        loadGate.SetResult(new GetPinnedResult { Revision = 50 });
        await refreshing;
        Assert.AreEqual(50UL, vm.CurrentRevision);
        Assert.IsFalse(vm.IsLoading);
    }

    [TestMethod]
    public async Task TestScopeWarningNotifiesBindingsWhenEditedAndReloaded()
    {
        var vm = new SortingViewModel(new MockAdminClient(), new NotificationBanner()) { QueryCode = "aaaa" };
        await vm.LoadPinnedAsync();
        var changed = new List<string?>();
        vm.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        vm.QueryCode = "bbbb";
        Assert.IsTrue(vm.IsQueryCodeDirty);
        Assert.Contains(nameof(vm.ScopeDirtyWarning), changed);
        StringAssert.Contains(vm.ScopeDirtyWarning, "aaaa");
        changed.Clear();
        await vm.LoadPinnedAsync();
        Assert.IsFalse(vm.IsQueryCodeDirty);
        Assert.AreEqual(string.Empty, vm.ScopeDirtyWarning);
        Assert.Contains(nameof(vm.ScopeDirtyWarning), changed);
    }

    [TestMethod]
    public async Task TestScopingPassesLayoutOnlyForShuangpin()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        string? capturedLayout = null;
        mock.OnGetPinned = (sch, code, lay, ct) =>
        {
            capturedLayout = lay;
            return Task.FromResult(new GetPinnedResult { Scheme = sch, Code = code, LayoutId = lay, Revision = 2 });
        };

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();
        Assert.IsNull(capturedLayout, "Wubi scheme must not pass layout_id");

        vm.SelectedScheme = InputSchemes.Shuangpin;
        vm.SelectedLayout = ShuangpinLayouts.Xiaohe;
        vm.QueryCode = "xl";
        await vm.LoadPinnedAsync();
        Assert.AreEqual(ShuangpinLayouts.Xiaohe, capturedLayout, "Shuangpin scheme must pass layout_id");
    }

    [TestMethod]
    public async Task TestConcurrencyRevisionConflictProtection()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        ulong? capturedExpectedRev = null;
        mock.OnGetPinned = (sch, code, lay, ct) =>
        {
            return Task.FromResult(new GetPinnedResult
            {
                Scheme = sch,
                Code = code,
                Revision = 42,
                Entries = [
                    new PinnedItem { EntryId = "e1", Text = "第一", OrderRank = 1 },
                    new PinnedItem { EntryId = "e2", Text = "第二", OrderRank = 2 }
                ]
            });
        };

        mock.OnReorderPinned = (sch, code, ids, lay, expRev, ct) =>
        {
            capturedExpectedRev = expRev;
            // Another window already saved rev 43!
            throw new AdminException(AdminErrorCode.RevisionConflict, "Revision conflict");
        };

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Pinyin,
            QueryCode = "kaixin"
        };
        await vm.LoadPinnedAsync();

        Assert.AreEqual(42ul, vm.CurrentRevision);
        Assert.AreEqual(2, vm.PinnedEntries.Count);

        // User moves "第二" up
        vm.SelectedPinned = vm.PinnedEntries[1];
        vm.MoveUp();
        Assert.AreEqual("第二", vm.PinnedEntries[0].Text);

        // User saves
        await vm.SaveSortOrderAsync();

        // 1. Expected revision was passed
        Assert.AreEqual(42ul, capturedExpectedRev);

        // 2. Banner reports conflict
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Error, banner.Type);
        StringAssert.Contains(banner.Message, "并发版本冲突");

        // 3. Draft order in UI is preserved for user to review
        Assert.AreEqual("第二", vm.PinnedEntries[0].Text);
    }

    [TestMethod]
    public async Task TestSaveTimeoutPreservesOrderWithoutAutoRetry()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (sch, code, lay, ct) =>
        {
            return Task.FromResult(new GetPinnedResult
            {
                Scheme = sch,
                Code = code,
                Revision = 1,
                Entries = [new PinnedItem { EntryId = "p1", Text = "置顶项", OrderRank = 1 }]
            });
        };

        mock.OnReorderPinned = (sch, code, ids, lay, expRev, ct) =>
        {
            throw new TimeoutException("请求超时，结果尚未确认");
        };

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();

        await vm.SaveSortOrderAsync();

        Assert.AreEqual(1, mock.ReorderPinnedCallCount, "Strictly no auto-retry on mutating save timeout!");
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, banner.Type);
        StringAssert.Contains(banner.Message, "结果尚未确认");
    }

    [TestMethod]
    public async Task TestABACrossSchemeDraftIsolationAndStaleLateArrival()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 1,
            Entries = [new PinnedItem { EntryId = "a1", Text = "原条目", OrderRank = 1 }]
        });

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();
        Assert.AreEqual(1ul, vm.CurrentRevision);

        // Start save draft 1, but hold it in-flight
        var saveGate = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnReorderPinned = (s, c, ids, l, r, ct) => saveGate.Task;
        var inFlightSave = vm.SaveSortOrderAsync();

        // Switch to Pinyin zhongwen
        vm.SelectedScheme = InputSchemes.Pinyin;
        vm.QueryCode = "zhongwen";
        await vm.LoadPinnedAsync();

        // Switch back to Wubi86 aaaa, which loads fresh draft (rev 50)
        vm.SelectedScheme = InputSchemes.Wubi86;
        vm.QueryCode = "aaaa";
        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 50,
            Entries = [new PinnedItem { EntryId = "new_a", Text = "新条目", OrderRank = 1 }]
        });
        await vm.LoadPinnedAsync();
        Assert.AreEqual(50ul, vm.CurrentRevision);
        Assert.AreEqual("new_a", vm.PinnedEntries[0].EntryId);

        // Now the old save completes with rev 2
        saveGate.SetResult(2);
        await inFlightSave;

        // Old save MUST NOT overwrite rev 50 or false success banner!
        Assert.AreEqual(50ul, vm.CurrentRevision, "Old draft save response must not overwrite current draft revision");
        Assert.AreEqual("new_a", vm.PinnedEntries[0].EntryId);
        StringAssert.Contains(banner.Message, "rev.50", "Banner must not be overwritten with old save confirmation");
    }

    [TestMethod]
    public async Task TestSameScopeRefreshSupersedesInFlightSave()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 1,
            Entries = [new PinnedItem { EntryId = "item1", Text = "初版", OrderRank = 1 }]
        });

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();

        // Start save, held in flight
        var saveGate = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnReorderPinned = (s, c, ids, l, r, ct) => saveGate.Task;
        var inFlightSave = vm.SaveSortOrderAsync();

        // Refresh same scope with rev 10
        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 10,
            Entries = [new PinnedItem { EntryId = "item2", Text = "服务端更新版", OrderRank = 1 }]
        });
        await vm.LoadPinnedAsync();
        Assert.AreEqual(10ul, vm.CurrentRevision);

        // Old save finishes with rev 2
        saveGate.SetResult(2);
        await inFlightSave;

        // Must remain rev 10
        Assert.AreEqual(10ul, vm.CurrentRevision);
        Assert.AreEqual("item2", vm.PinnedEntries[0].EntryId);
        StringAssert.Contains(banner.Message, "rev.10");
    }

    [TestMethod]
    public async Task TestLateConflictOrTimeoutDoesNotPolluteSubsequentDraft()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 5,
            Entries = [new PinnedItem { EntryId = "k1", Text = "开心", OrderRank = 1 }]
        });

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Pinyin,
            QueryCode = "kaixin"
        };
        await vm.LoadPinnedAsync();

        var saveGate = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnReorderPinned = (s, c, ids, l, r, ct) => saveGate.Task;
        var inFlightSave = vm.SaveSortOrderAsync();

        // User switches to kuailu
        vm.QueryCode = "kuailu";
        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 8,
            Entries = [new PinnedItem { EntryId = "k2", Text = "快乐", OrderRank = 1 }]
        });
        await vm.LoadPinnedAsync();
        Assert.AreEqual(8ul, vm.CurrentRevision);

        // Previous save fails with RevisionConflict
        saveGate.SetException(new AdminException(AdminErrorCode.RevisionConflict, "Conflict"));
        await inFlightSave;

        // Error banner must NOT display conflict on the new kuailu scope
        Assert.AreEqual(8ul, vm.CurrentRevision);
        Assert.IsFalse(banner.Message.Contains("并发版本冲突"), "Obsolete save failure must not pollute current draft banner");
    }

    [TestMethod]
    public async Task TestSaveWithConcurrentEditsReportsSubsequentModifications()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 1,
            Entries = [
                new PinnedItem { EntryId = "e1", Text = "一", OrderRank = 1 },
                new PinnedItem { EntryId = "e2", Text = "二", OrderRank = 2 }
            ]
        });

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();

        var saveGate = new TaskCompletionSource<ulong>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnReorderPinned = (s, c, ids, l, r, ct) => saveGate.Task;
        var inFlightSave = vm.SaveSortOrderAsync();

        // While save is in flight, user modifies the list (e.g. moves "二" up)
        vm.SelectedPinned = vm.PinnedEntries[1];
        vm.MoveUp();
        Assert.AreEqual("二", vm.PinnedEntries[0].Text);

        // Save completes
        saveGate.SetResult(2);
        await inFlightSave;

        // Revision is updated to 2, but banner accurately reports that subsequent edits are uncommitted!
        Assert.AreEqual(2ul, vm.CurrentRevision);
        Assert.IsTrue(banner.IsVisible);
        StringAssert.Contains(banner.Message, "当前列表包含尚未保存的后续修改");
    }

    [TestMethod]
    public async Task TestInvalidatedLoadCleansUpIsLoading()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        var loadGate = new TaskCompletionSource<GetPinnedResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        mock.OnGetPinned = (s, c, l, ct) => loadGate.Task;

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };

        var inFlightLoad = vm.LoadPinnedAsync();
        Assert.IsTrue(vm.IsLoading, "IsLoading must be true while load is active");

        // User switches scheme while load is running
        vm.SelectedScheme = InputSchemes.Pinyin;
        Assert.IsFalse(vm.IsLoading, "InvalidateCurrentScope must immediately reset IsLoading to false");

        // Old load completes
        loadGate.SetResult(new GetPinnedResult());
        await inFlightLoad;

        Assert.IsFalse(vm.IsLoading, "Completed discarded load must not leave IsLoading as true");
    }

    [TestMethod]
    public async Task TestScopeDirtyWarningWhenQueryCodeEditedWithoutReload()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 1,
            Entries = [new PinnedItem { EntryId = "e1", Text = "第一", OrderRank = 1 }]
        });

        var vm = new SortingViewModel(mock, banner)
        {
            SelectedScheme = InputSchemes.Wubi86,
            QueryCode = "aaaa"
        };
        await vm.LoadPinnedAsync();

        Assert.IsFalse(vm.IsQueryCodeDirty);
        Assert.AreEqual(string.Empty, vm.ScopeDirtyWarning);
        StringAssert.StartsWith(vm.RevisionDisplay, "wubi86:aaaa (rev.1)");
        Assert.IsFalse(vm.RevisionDisplay.Contains("(未重新加载)"));

        // User changes query code to "bbbb" without clicking load
        vm.QueryCode = "bbbb";
        Assert.IsTrue(vm.IsQueryCodeDirty);
        StringAssert.Contains(vm.RevisionDisplay, "(未重新加载)");
        StringAssert.Contains(vm.ScopeDirtyWarning, "aaaa");
        StringAssert.Contains(vm.ScopeDirtyWarning, "bbbb");

        // Reload for bbbb
        mock.OnGetPinned = (s, c, l, ct) => Task.FromResult(new GetPinnedResult
        {
            Revision = 2,
            Entries = []
        });
        await vm.LoadPinnedAsync();
        Assert.IsFalse(vm.IsQueryCodeDirty);
        Assert.AreEqual(string.Empty, vm.ScopeDirtyWarning);
    }
}

