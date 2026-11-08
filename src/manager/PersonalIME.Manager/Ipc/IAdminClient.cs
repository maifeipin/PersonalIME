using System.Text.Json;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.Ipc;

public interface IAdminClient
{
    bool IsConnected { get; }
    string PipeName { get; }

    Task<JsonElement> SendRequestAsync(string action, object? parameters = null, CancellationToken ct = default);
    Task<ServiceStatusInfo> GetStatusAsync(CancellationToken ct = default);
    Task<InputPolicyConfig> GetPolicyAsync(CancellationToken ct = default);
    Task<uint> SavePolicyAsync(InputPolicyConfig policy, uint expectedRevision, CancellationToken ct = default);
    Task<GetEntriesResult> GetEntriesAsync(string? keyword, string? scheme, int page, int pageSize, CancellationToken ct = default);
    Task<string> AddEntryAsync(string text, IEnumerable<EntryCodeItem> codes, CancellationToken ct = default);
    Task UpdateEntryAsync(string entryId, string? text, IEnumerable<EntryCodeItem>? codes, CancellationToken ct = default);
    Task DeleteEntryAsync(string entryId, CancellationToken ct = default);
    Task<GetPinnedResult> GetPinnedAsync(string scheme, string code, string? layoutId = null, CancellationToken ct = default);
    Task<ulong> SetPinAsync(string scheme, string code, string entryId, bool isPinned, int orderRank, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default);
    Task<ulong> ReorderPinnedAsync(string scheme, string code, IEnumerable<string> entryIds, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default);
    Task<List<ScopeCandidateItem>> GetScopeCandidatesAsync(string scheme, string code, string? layoutId = null, string? keyword = null, CancellationToken ct = default);
    Task ExportDataAsync(string filePath, CancellationToken ct = default);
    Task ImportDataAsync(string filePath, string mode, CancellationToken ct = default);
    Task ResetStatsAsync(string? entryId = null, CancellationToken ct = default);
    Task<ReverseLookupResultItem> ReverseLookupAsync(string text, string targetMethod, string? shuangpinLayout = null, CancellationToken ct = default);
    Task<List<PluginModel>> GetPluginsAsync(bool onlyEnabled = false, CancellationToken ct = default);
    Task<long> AddPluginAsync(PluginModel plugin, CancellationToken ct = default);
    Task UpdatePluginAsync(PluginModel plugin, CancellationToken ct = default);
    Task DeletePluginAsync(long id, CancellationToken ct = default);
    Task ExecutePluginAsync(long id, CancellationToken ct = default);
    Task<List<PluginLogModel>> GetPluginLogsAsync(long? pluginId = null, int limit = 50, CancellationToken ct = default);
    Task ClearPluginLogsAsync(long? pluginId = null, CancellationToken ct = default);
    Task ShutdownAsync(CancellationToken ct = default);
}

