using System.Text.Json;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.Tests;

public class MockAdminClient : IAdminClient
{
    public bool IsConnected { get; set; } = true;
    public string PipeName => "MockPipe";

    public int GetStatusCallCount { get; private set; }
    public int GetPolicyCallCount { get; private set; }
    public int SavePolicyCallCount { get; private set; }
    public int GetEntriesCallCount { get; private set; }
    public int AddEntryCallCount { get; private set; }
    public int UpdateEntryCallCount { get; private set; }
    public int DeleteEntryCallCount { get; private set; }
    public int GetPinnedCallCount { get; private set; }
    public int ReorderPinnedCallCount { get; private set; }
    public int ExportDataCallCount { get; private set; }
    public int ImportDataCallCount { get; private set; }
    public int ResetStatsCallCount { get; private set; }

    public Func<CancellationToken, Task<ServiceStatusInfo>>? OnGetStatus { get; set; }
    public Func<CancellationToken, Task<InputPolicyConfig>>? OnGetPolicy { get; set; }
    public Func<InputPolicyConfig, uint, CancellationToken, Task<uint>>? OnSavePolicy { get; set; }
    public Func<string?, string?, int, int, CancellationToken, Task<GetEntriesResult>>? OnGetEntries { get; set; }
    public Func<string, IEnumerable<EntryCodeItem>, CancellationToken, Task<string>>? OnAddEntry { get; set; }
    public Func<string, string?, IEnumerable<EntryCodeItem>?, CancellationToken, Task>? OnUpdateEntry { get; set; }
    public Func<string, CancellationToken, Task>? OnDeleteEntry { get; set; }
    public Func<string, string, string?, CancellationToken, Task<GetPinnedResult>>? OnGetPinned { get; set; }
    public Func<string, string, IEnumerable<string>, string?, ulong?, CancellationToken, Task<ulong>>? OnReorderPinned { get; set; }
    public Func<string, string, string, bool, int, string?, ulong?, CancellationToken, Task<ulong>>? OnSetPin { get; set; }
    public Func<string, string, string?, string?, CancellationToken, Task<List<ScopeCandidateItem>>>? OnGetScopeCandidates { get; set; }
    public Func<string, CancellationToken, Task>? OnExportData { get; set; }
    public Func<string, string, CancellationToken, Task>? OnImportData { get; set; }
    public Func<string?, CancellationToken, Task>? OnResetStats { get; set; }
    public Func<string, string, string?, CancellationToken, Task<ReverseLookupResultItem>>? OnReverseLookup { get; set; }
    public int ReverseLookupCallCount { get; private set; }

    public Task<JsonElement> SendRequestAsync(string action, object? parameters = null, CancellationToken ct = default)
    {
        return Task.FromResult(default(JsonElement));
    }

    public async Task<ServiceStatusInfo> GetStatusAsync(CancellationToken ct = default)
    {
        GetStatusCallCount++;
        if (OnGetStatus != null) return await OnGetStatus(ct);
        return new ServiceStatusInfo { ServiceVersion = "1.0.0", ActiveClients = 1 };
    }

    public async Task<InputPolicyConfig> GetPolicyAsync(CancellationToken ct = default)
    {
        GetPolicyCallCount++;
        if (OnGetPolicy != null) return await OnGetPolicy(ct);
        return new InputPolicyConfig { Revision = 1, CandidatePageSize = 5 };
    }

    public async Task<uint> SavePolicyAsync(InputPolicyConfig policy, uint expectedRevision, CancellationToken ct = default)
    {
        SavePolicyCallCount++;
        if (OnSavePolicy != null) return await OnSavePolicy(policy, expectedRevision, ct);
        return expectedRevision + 1;
    }

    public async Task<GetEntriesResult> GetEntriesAsync(string? keyword, string? scheme, int page, int pageSize, CancellationToken ct = default)
    {
        GetEntriesCallCount++;
        if (OnGetEntries != null) return await OnGetEntries(keyword, scheme, page, pageSize, ct);
        return new GetEntriesResult { Total = 0, Page = page, PageSize = pageSize, Entries = [] };
    }

    public async Task<string> AddEntryAsync(string text, IEnumerable<EntryCodeItem> codes, CancellationToken ct = default)
    {
        AddEntryCallCount++;
        if (OnAddEntry != null) return await OnAddEntry(text, codes, ct);
        return "mock_entry_id_1";
    }

    public async Task UpdateEntryAsync(string entryId, string? text, IEnumerable<EntryCodeItem>? codes, CancellationToken ct = default)
    {
        UpdateEntryCallCount++;
        if (OnUpdateEntry != null) await OnUpdateEntry(entryId, text, codes, ct);
    }

    public async Task DeleteEntryAsync(string entryId, CancellationToken ct = default)
    {
        DeleteEntryCallCount++;
        if (OnDeleteEntry != null) await OnDeleteEntry(entryId, ct);
    }

    public async Task<GetPinnedResult> GetPinnedAsync(string scheme, string code, string? layoutId = null, CancellationToken ct = default)
    {
        GetPinnedCallCount++;
        if (OnGetPinned != null) return await OnGetPinned(scheme, code, layoutId, ct);
        return new GetPinnedResult { Scheme = scheme, Code = code, LayoutId = layoutId, Revision = 1, Entries = [] };
    }

    public async Task<ulong> SetPinAsync(string scheme, string code, string entryId, bool isPinned, int orderRank, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default)
    {
        if (OnSetPin != null) return await OnSetPin(scheme, code, entryId, isPinned, orderRank, layoutId, expectedRevision, ct);
        return expectedRevision.GetValueOrDefault(0) + 1;
    }

    public async Task<ulong> ReorderPinnedAsync(string scheme, string code, IEnumerable<string> entryIds, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default)
    {
        ReorderPinnedCallCount++;
        if (OnReorderPinned != null) return await OnReorderPinned(scheme, code, entryIds, layoutId, expectedRevision, ct);
        return expectedRevision.GetValueOrDefault(0) + 1;
    }

    public async Task<List<ScopeCandidateItem>> GetScopeCandidatesAsync(string scheme, string code, string? layoutId = null, string? keyword = null, CancellationToken ct = default)
    {
        if (OnGetScopeCandidates != null) return await OnGetScopeCandidates(scheme, code, layoutId, keyword, ct);
        return [];
    }

    public async Task ExportDataAsync(string filePath, CancellationToken ct = default)
    {
        ExportDataCallCount++;
        if (OnExportData != null) await OnExportData(filePath, ct);
    }

    public async Task ImportDataAsync(string filePath, string mode, CancellationToken ct = default)
    {
        ImportDataCallCount++;
        if (OnImportData != null) await OnImportData(filePath, mode, ct);
    }

    public async Task ResetStatsAsync(string? entryId = null, CancellationToken ct = default)
    {
        ResetStatsCallCount++;
        if (OnResetStats != null) await OnResetStats(entryId, ct);
    }

    public async Task<ReverseLookupResultItem> ReverseLookupAsync(string text, string targetMethod, string? shuangpinLayout = null, CancellationToken ct = default)
    {
        ReverseLookupCallCount++;
        if (OnReverseLookup != null)
        {
            return await OnReverseLookup(text, targetMethod, shuangpinLayout, ct);
        }
        return new ReverseLookupResultItem
        {
            Found = true,
            Text = text,
            TargetMethod = targetMethod,
            IsPhrase = true,
            PrimaryCode = "mock_code",
            Code = "mock_code",
            AllCodes = ["mock_code"]
        };
    }
}

