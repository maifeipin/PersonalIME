using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class WordbookViewModelTests
{
    [TestMethod]
    public async Task TestSearchCancellationOfSupersededQueries()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var tcsSlow = new TaskCompletionSource<GetEntriesResult>();

        int queryCount = 0;
        mock.OnGetEntries = (kw, sch, p, ps, ct) =>
        {
            queryCount++;
            if (kw == "first")
            {
                // Slow first query
                return tcsSlow.Task;
            }
            return Task.FromResult(new GetEntriesResult
            {
                Total = 1,
                Entries = [new UserEntryItem { Id = "fast_1", Text = "第二查询结果" }]
            });
        };

        var vm = new WordbookViewModel(mock, banner);

        // Trigger first slow query
        vm.SearchKeyword = "first";

        // Immediately trigger second fast query
        vm.SearchKeyword = "second";

        // Complete the fast query and wait
        await Task.Delay(50);

        Assert.AreEqual(1, vm.Entries.Count);
        Assert.AreEqual("第二查询结果", vm.Entries[0].Text);

        // Cancel the slow one safely
        tcsSlow.TrySetCanceled();
    }

    [TestMethod]
    public async Task TestSaveTimeoutPreservesDraftWithoutAutoRetry()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        // Simulate save timeout
        mock.OnAddEntry = (text, codes, ct) =>
        {
            throw new TimeoutException("请求超时，结果尚未确认");
        };

        var vm = new WordbookViewModel(mock, banner);
        vm.OpenAddDialogCommand.Execute(null);

        vm.EditorText = "待保存重要草稿（包含 Emoji 🚀 和换行\n第二行）";
        vm.NewCodeScheme = InputSchemes.Wubi86;
        vm.NewCodeText = "aaaa";
        vm.AddCodeToDraftCommand.Execute(null);

        // User clicks save
        await vm.SaveEditorAsync();

        // 1. Editor must stay open
        Assert.IsTrue(vm.IsEditorOpen, "Editor must remain open on save timeout");

        // 2. Draft text and codes must be preserved
        Assert.AreEqual("待保存重要草稿（包含 Emoji 🚀 和换行\n第二行）", vm.EditorText);
        Assert.AreEqual(1, vm.EditorCodes.Count);
        Assert.AreEqual("aaaa", vm.EditorCodes[0].Code);

        // 3. Banner displays unconfirmed prompt
        Assert.IsTrue(banner.IsVisible);
        Assert.AreEqual(BannerType.Warning, banner.Type);
        StringAssert.Contains(banner.Message, "结果尚未确认");

        // 4. Prohibit auto-retry: AddEntry was called exactly once!
        Assert.AreEqual(1, mock.AddEntryCallCount, "Auto-retry is strictly prohibited on mutating calls!");
    }

    [TestMethod]
    public async Task TestMultiCodeAdditionAndDraftManagement()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        string? savedText = null;
        List<EntryCodeItem>? savedCodes = null;

        mock.OnAddEntry = (text, codes, ct) =>
        {
            savedText = text;
            savedCodes = codes.ToList();
            return Task.FromResult("entry_101");
        };

        var vm = new WordbookViewModel(mock, banner);
        vm.OpenAddDialogCommand.Execute(null);

        vm.EditorText = "西安";

        // Add Wubi code
        vm.NewCodeScheme = InputSchemes.Wubi86;
        vm.NewCodeText = "sq";
        vm.AddCodeToDraftCommand.Execute(null);

        // Add Pinyin code 1
        vm.NewCodeScheme = InputSchemes.Pinyin;
        vm.NewCodeText = "xian";
        vm.AddCodeToDraftCommand.Execute(null);

        // Add Pinyin code 2
        vm.NewCodeScheme = InputSchemes.Pinyin;
        vm.NewCodeText = "xi'an";
        vm.AddCodeToDraftCommand.Execute(null);

        Assert.AreEqual(3, vm.EditorCodes.Count);

        await vm.SaveEditorAsync();

        Assert.IsFalse(vm.IsEditorOpen);
        Assert.AreEqual("西安", savedText);
        Assert.IsNotNull(savedCodes);
        Assert.AreEqual(3, savedCodes.Count);
        Assert.IsTrue(savedCodes.Any(c => c.Scheme == "pinyin" && c.Code == "xi'an"));
    }

    [TestMethod]
    public async Task TestCheckReverseLookupCommand()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var vm = new WordbookViewModel(mock, banner);

        // 1. Text is empty
        vm.EditorText = "";
        await vm.CheckReverseLookupAsync();
        Assert.AreEqual("请先输入词条正文", vm.ReverseLookupPreview);

        // 2. Found whole-word
        mock.OnReverseLookup = (text, method, layout, ct) => Task.FromResult(new ReverseLookupResultItem
        {
            Found = true,
            Text = text,
            TargetMethod = method,
            IsPhrase = true,
            PrimaryCode = "aaaa",
            AllCodes = ["aaaa", "a"]
        });

        vm.EditorText = "工";
        vm.NewCodeScheme = InputSchemes.Wubi86;
        await vm.CheckReverseLookupAsync();
        Assert.AreEqual(1, mock.ReverseLookupCallCount);
        Assert.AreEqual("反查命中: aaaa (整词编码)", vm.ReverseLookupPreview);

        // 3. Found decomposition
        mock.OnReverseLookup = (text, method, layout, ct) => Task.FromResult(new ReverseLookupResultItem
        {
            Found = true,
            Text = text,
            TargetMethod = method,
            IsPhrase = false,
            PrimaryCode = "aaaa adwe aaaa",
            CharCodes = ["aaaa", "adwe", "aaaa"]
        });

        vm.EditorText = "工期工";
        await vm.CheckReverseLookupAsync();
        Assert.AreEqual("反查命中: aaaa adwe aaaa (逐字拆码)", vm.ReverseLookupPreview);

        // 4. Not found (no guessing)
        mock.OnReverseLookup = (text, method, layout, ct) => Task.FromResult(new ReverseLookupResultItem
        {
            Found = false,
            Text = text,
            TargetMethod = method
        });

        vm.EditorText = "工⚡";
        await vm.CheckReverseLookupAsync();
        Assert.AreEqual("反查无结果 (查不到不猜造)", vm.ReverseLookupPreview);
    }

    [TestMethod]
    public async Task TestReverseLookupSessionIsolationStaleResponseDiscarded()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var gate = new TaskCompletionSource<ReverseLookupResultItem>(TaskCreationOptions.RunContinuationsAsynchronously);

        mock.OnReverseLookup = (text, target, layout, ct) => gate.Task;

        var vm = new WordbookViewModel(mock, banner) { EditorText = "OLD" };
        var pending = vm.CheckReverseLookupAsync();

        // User switches to new editor session and changes text
        vm.OpenAddDialogCommand.Execute(null);
        vm.EditorText = "NEW";

        // Stale response from OLD completes
        gate.SetResult(new ReverseLookupResultItem
        {
            Found = true,
            IsPhrase = true,
            PrimaryCode = "aaaa",
            Code = "aaaa"
        });

        await pending;

        // Preview must NOT be polluted with stale "OLD" result
        Assert.AreEqual(string.Empty, vm.ReverseLookupPreview, "Stale reverse lookup response must be cleanly discarded");
    }

    [TestMethod]
    public async Task TestReverseLookupRawEdgePreservedNoTrim()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();

        string? queriedText = null;
        mock.OnReverseLookup = (text, target, layout, ct) =>
        {
            queriedText = text;
            return Task.FromResult(new ReverseLookupResultItem
            {
                Found = true,
                IsPhrase = true,
                PrimaryCode = "zhong",
                Code = "zhong"
            });
        };

        var vm = new WordbookViewModel(mock, banner);
        vm.OpenAddDialogCommand.Execute(null);

        // Raw text with boundary whitespace
        vm.EditorText = "  EDGE  ";
        vm.NewCodeScheme = InputSchemes.Pinyin;

        await vm.CheckReverseLookupAsync();

        // Must preserve raw text without Trim()
        Assert.AreEqual("  EDGE  ", queriedText, "ReverseLookup must preserve raw text without trimming leading/trailing whitespace");
        Assert.AreEqual("反查命中: zhong (整词编码)", vm.ReverseLookupPreview);
    }

    [TestMethod]
    public async Task TestReverseLookupEditorCloseCancelsPreview()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var tcs = new TaskCompletionSource<ReverseLookupResultItem>(TaskCreationOptions.RunContinuationsAsynchronously);

        mock.OnReverseLookup = (text, target, layout, ct) => tcs.Task;

        var vm = new WordbookViewModel(mock, banner);
        vm.OpenAddDialogCommand.Execute(null);
        vm.EditorText = "CLOSING";

        var pending = vm.CheckReverseLookupAsync();

        // User closes editor
        vm.CloseEditorCommand.Execute(null);
        Assert.IsFalse(vm.IsEditorOpen);
        Assert.AreEqual(string.Empty, vm.ReverseLookupPreview);

        // Later response arrives
        tcs.SetResult(new ReverseLookupResultItem
        {
            Found = true,
            IsPhrase = true,
            PrimaryCode = "close_code"
        });

        await pending;

        Assert.AreEqual(string.Empty, vm.ReverseLookupPreview, "Closed editor must not receive late preview update");
    }

    [TestMethod]
    public async Task TestReverseLookupTextChangedSupersedesOldResponse()
    {
        var mock = new MockAdminClient();
        var banner = new NotificationBanner();
        var tcsOld = new TaskCompletionSource<ReverseLookupResultItem>(TaskCreationOptions.RunContinuationsAsynchronously);

        mock.OnReverseLookup = (text, target, layout, ct) => tcsOld.Task;

        var vm = new WordbookViewModel(mock, banner);
        vm.OpenAddDialogCommand.Execute(null);
        vm.EditorText = "TEXT_ONE";

        var pending = vm.CheckReverseLookupAsync();

        // User changes text to TEXT_TWO
        vm.EditorText = "TEXT_TWO";
        Assert.AreEqual(string.Empty, vm.ReverseLookupPreview);

        // Old response resolves
        tcsOld.SetResult(new ReverseLookupResultItem
        {
            Found = true,
            IsPhrase = true,
            PrimaryCode = "code_one"
        });

        await pending;

        // Must still be empty because text changed
        Assert.AreEqual(string.Empty, vm.ReverseLookupPreview, "Old text response must not fill preview after text mutation");
    }
}

