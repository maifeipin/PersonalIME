using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.Ipc;

public class AdminIpcClient : IAdminClient
{
    public const string DefaultPipeName = "PersonalIME_Admin_Pipe";
    public const int DefaultTimeoutMs = 5000;

    private readonly string _pipeName;
    private int _requestIdCounter;
    internal CancellationToken LifetimeCancellationToken { get; set; }

    public string PipeName => _pipeName;
    public bool IsConnected { get; private set; }

    public AdminIpcClient(string pipeName = DefaultPipeName)
    {
        _pipeName = pipeName;
    }

    public async Task<JsonElement> SendRequestAsync(string action, object? parameters = null, CancellationToken ct = default)
    {
        uint reqId = (uint)Interlocked.Increment(ref _requestIdCounter);

        var reqObj = new JsonObject
        {
            ["action"] = action
        };
        if (parameters != null)
        {
            reqObj["params"] = JsonSerializer.SerializeToNode(parameters);
        }

        string jsonPayload = reqObj.ToJsonString();
        byte[] payloadBytes = Encoding.UTF8.GetBytes(jsonPayload);

        var lifetimeToken = LifetimeCancellationToken;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct, lifetimeToken);
        cts.CancelAfter(DefaultTimeoutMs);

        bool hasSentRequest = false;
        try
        {
            await using var pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(DefaultTimeoutMs, cts.Token);
            IsConnected = true;

            var header = AdminHeader.Create(reqId, (uint)payloadBytes.Length);
            byte[] headerBytes = new byte[AdminHeader.HeaderSize];
            header.WriteTo(headerBytes);

            // Write header + payload
            await pipe.WriteAsync(headerBytes, cts.Token);
            if (payloadBytes.Length > 0)
            {
                await pipe.WriteAsync(payloadBytes, cts.Token);
            }
            await pipe.FlushAsync(cts.Token);
            hasSentRequest = true;

            // Read 16-byte response header
            byte[] respHeaderBytes = new byte[AdminHeader.HeaderSize];
            await ReadExactBytesAsync(pipe, respHeaderBytes, cts.Token);
            var respHeader = AdminHeader.ReadFrom(respHeaderBytes);

            if (!respHeader.IsValidMagic())
            {
                throw new AdminException(AdminErrorCode.InvalidMagic, "服务端响应协议魔数无效");
            }
            if (respHeader.PayloadLength > AdminHeader.MaxPayloadSize)
            {
                throw new AdminException(AdminErrorCode.PayloadTooLarge, $"服务端响应过大 ({respHeader.PayloadLength} 字节)");
            }

            // Read response payload
            byte[] respPayloadBytes = new byte[respHeader.PayloadLength];
            if (respHeader.PayloadLength > 0)
            {
                await ReadExactBytesAsync(pipe, respPayloadBytes, cts.Token);
            }

            if (respHeader.Version != AdminHeader.ProtocolVersion1)
            {
                throw new AdminException(AdminErrorCode.UnsupportedVersion, $"服务端响应协议版本无效: 0x{respHeader.Version:X4}");
            }
            if (respHeader.RequestId != reqId)
            {
                throw new AdminException(AdminErrorCode.ValidationFailed, $"服务端响应请求ID不匹配 (预期 {reqId}, 实际 {respHeader.RequestId})");
            }

            string respJson = Encoding.UTF8.GetString(respPayloadBytes);
            using var doc = JsonDocument.Parse(respJson);
            var root = doc.RootElement.Clone();

            if (root.ValueKind != JsonValueKind.Object)
            {
                throw new AdminException(AdminErrorCode.InvalidJson, "响应内容不是有效的 JSON 对象");
            }

            if (!root.TryGetProperty("success", out var successProp) ||
                (successProp.ValueKind != JsonValueKind.True && successProp.ValueKind != JsonValueKind.False))
            {
                throw new AdminException(AdminErrorCode.ValidationFailed, "响应缺少必需的布尔型 success 字段");
            }

            if (!successProp.GetBoolean())
            {
                var errorCode = AdminErrorCode.ValidationFailed;
                string errorMessage = "服务端处理失败";

                if (root.TryGetProperty("error", out var errorObj))
                {
                    if (errorObj.TryGetProperty("code", out var codeProp))
                    {
                        errorCode = (AdminErrorCode)codeProp.GetInt32();
                    }
                    if (errorObj.TryGetProperty("message", out var msgProp))
                    {
                        errorMessage = msgProp.GetString() ?? errorMessage;
                    }
                }
                throw new AdminException(errorCode, errorMessage);
            }

            return root.TryGetProperty("data", out var dataProp) ? dataProp : root;
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested || lifetimeToken.IsCancellationRequested)
        {
            IsConnected = false;
            throw;
        }
        catch (OperationCanceledException ex)
        {
            IsConnected = false;
            if (hasSentRequest)
            {
                throw new AdminUnconfirmedException("请求已发出但连接中断/超时，结果尚未确认", ex);
            }
            throw new TimeoutException("连接超时");
        }
        catch (AdminException)
        {
            IsConnected = false;
            throw;
        }
        catch (Exception ex)
        {
            IsConnected = false;
            if (hasSentRequest)
            {
                throw new AdminUnconfirmedException("请求已发出但连接中断/超时，结果尚未确认", ex);
            }
            throw;
        }
    }

    private static async Task ReadExactBytesAsync(NamedPipeClientStream stream, byte[] buffer, CancellationToken ct)
    {
        int totalRead = 0;
        while (totalRead < buffer.Length)
        {
            int read = await stream.ReadAsync(buffer.AsMemory(totalRead, buffer.Length - totalRead), ct);
            if (read == 0)
                throw new EndOfStreamException("管道在读取完整数据帧前意外关闭");
            totalRead += read;
        }
    }

    public async Task<ServiceStatusInfo> GetStatusAsync(CancellationToken ct = default)
    {
        var data = await SendRequestAsync("GetStatus", null, ct);
        return JsonSerializer.Deserialize<ServiceStatusInfo>(data.GetRawText()) ?? new();
    }

    public async Task<InputPolicyConfig> GetPolicyAsync(CancellationToken ct = default)
    {
        var data = await SendRequestAsync("GetPolicy", null, ct);
        if (data.TryGetProperty("policy", out var policyProp))
        {
            return JsonSerializer.Deserialize<InputPolicyConfig>(policyProp.GetRawText()) ?? new();
        }
        return JsonSerializer.Deserialize<InputPolicyConfig>(data.GetRawText()) ?? new();
    }

    public async Task<uint> SavePolicyAsync(InputPolicyConfig policy, uint expectedRevision, CancellationToken ct = default)
    {
        var parameters = new
        {
            expected_revision = expectedRevision,
            policy = new
            {
                candidate_page_size = policy.CandidatePageSize,
                primary_input_method = policy.PrimaryInputMethod,
                shuangpin_layout = ShuangpinLayouts.Canonicalize(policy.ShuangpinLayout),
                frequency_enabled = policy.FrequencyEnabled,
                learning_enabled = policy.LearningEnabled,
                mixed_input_enabled = policy.MixedInputEnabled,
                initial_mode = new
                {
                    chinese = policy.InitialMode?.Chinese ?? true,
                    full_width = policy.InitialMode?.FullWidth ?? false,
                    chinese_punctuation = policy.InitialMode != null ? policy.InitialMode.ChinesePunctuation : true
                },
                input_method_order = policy.MethodPriorities,
                enabled_input_methods = policy.EnabledInputMethods ?? policy.MethodPriorities,
                input_methods = policy.InputMethods != null ? (object)policy.InputMethods.Select(m => new { scheme = m.Scheme, enabled = m.Enabled }) : (object)policy.MethodPriorities.Select(m => new { scheme = m, enabled = true }),
                dictionary_order = policy.DictionaryPriorities.Select(d => new { dict_id = d.DictId, enabled = d.IsEnabled })
            }
        };

        var data = await SendRequestAsync("SavePolicy", parameters, ct);
        if (data.TryGetProperty("revision", out var revProp))
        {
            return revProp.GetUInt32();
        }
        throw new AdminException(AdminErrorCode.ValidationFailed, "服务端未返回 revision");
    }

    public async Task<GetEntriesResult> GetEntriesAsync(string? keyword, string? scheme, int page, int pageSize, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["page"] = page,
            ["page_size"] = pageSize
        };
        if (!string.IsNullOrEmpty(keyword)) p["keyword"] = keyword;
        if (!string.IsNullOrEmpty(scheme)) p["scheme"] = scheme;

        var data = await SendRequestAsync("GetEntries", p, ct);
        return JsonSerializer.Deserialize<GetEntriesResult>(data.GetRawText()) ?? new();
    }

    public async Task<string> AddEntryAsync(string text, IEnumerable<EntryCodeItem> codes, CancellationToken ct = default)
    {
        var p = new
        {
            text,
            codes = codes.Select(c => new { scheme = c.Scheme, code = c.Code })
        };
        var data = await SendRequestAsync("AddEntry", p, ct);
        if (data.TryGetProperty("entry_id", out var idProp))
        {
            string id = idProp.GetString() ?? string.Empty;
            if (!string.IsNullOrEmpty(id)) return id;
        }
        throw new AdminException(AdminErrorCode.ValidationFailed, "服务端未返回 entry_id");
    }

    public async Task UpdateEntryAsync(string entryId, string? text, IEnumerable<EntryCodeItem>? codes, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["entry_id"] = entryId
        };
        if (text != null) p["text"] = text;
        if (codes != null)
        {
            p["codes"] = JsonSerializer.SerializeToNode(codes.Select(c => new { scheme = c.Scheme, code = c.Code }));
        }
        await SendRequestAsync("UpdateEntry", p, ct);
    }

    public async Task DeleteEntryAsync(string entryId, CancellationToken ct = default)
    {
        var p = new { entry_id = entryId };
        await SendRequestAsync("DeleteEntry", p, ct);
    }

    public async Task<GetPinnedResult> GetPinnedAsync(string scheme, string code, string? layoutId = null, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["scheme"] = scheme,
            ["code"] = code
        };
        if (!string.IsNullOrEmpty(layoutId)) p["layout_id"] = ShuangpinLayouts.Canonicalize(layoutId);

        var data = await SendRequestAsync("GetPinned", p, ct);
        return JsonSerializer.Deserialize<GetPinnedResult>(data.GetRawText()) ?? new();
    }

    public async Task<ulong> SetPinAsync(string scheme, string code, string entryId, bool isPinned, int orderRank, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["scheme"] = scheme,
            ["code"] = code,
            ["entry_id"] = entryId,
            ["is_pinned"] = isPinned,
            ["order_rank"] = orderRank
        };
        if (!string.IsNullOrEmpty(layoutId)) p["layout_id"] = ShuangpinLayouts.Canonicalize(layoutId);
        if (expectedRevision.HasValue) p["expected_revision"] = expectedRevision.Value;

        var data = await SendRequestAsync("SetPin", p, ct);
        if (data.TryGetProperty("revision", out var revProp))
        {
            return revProp.GetUInt64();
        }
        throw new AdminException(AdminErrorCode.ValidationFailed, "服务端未返回 revision");
    }

    public async Task<ulong> ReorderPinnedAsync(string scheme, string code, IEnumerable<string> entryIds, string? layoutId = null, ulong? expectedRevision = null, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["scheme"] = scheme,
            ["code"] = code,
            ["entry_ids"] = JsonSerializer.SerializeToNode(entryIds)
        };
        if (!string.IsNullOrEmpty(layoutId)) p["layout_id"] = ShuangpinLayouts.Canonicalize(layoutId);
        if (expectedRevision.HasValue) p["expected_revision"] = expectedRevision.Value;

        var data = await SendRequestAsync("ReorderPinned", p, ct);
        if (data.TryGetProperty("revision", out var revProp))
        {
            return revProp.GetUInt64();
        }
        throw new AdminException(AdminErrorCode.ValidationFailed, "服务端未返回 revision");
    }

    public async Task<List<ScopeCandidateItem>> GetScopeCandidatesAsync(string scheme, string code, string? layoutId = null, string? keyword = null, CancellationToken ct = default)
    {
        var p = new JsonObject
        {
            ["scheme"] = scheme,
            ["code"] = code
        };
        if (!string.IsNullOrEmpty(layoutId)) p["layout_id"] = ShuangpinLayouts.Canonicalize(layoutId);
        if (!string.IsNullOrEmpty(keyword)) p["keyword"] = keyword;

        var data = await SendRequestAsync("GetScopeCandidates", p, ct);
        if (data.TryGetProperty("candidates", out var candProp))
        {
            return JsonSerializer.Deserialize<List<ScopeCandidateItem>>(candProp.GetRawText()) ?? [];
        }
        return [];
    }

    public async Task ExportDataAsync(string filePath, CancellationToken ct = default)
    {
        var p = new { file_path = filePath };
        await SendRequestAsync("ExportData", p, ct);
    }

    public async Task ImportDataAsync(string filePath, string mode, CancellationToken ct = default)
    {
        var p = new { file_path = filePath, mode };
        await SendRequestAsync("ImportData", p, ct);
    }

    public async Task ResetStatsAsync(string? entryId = null, CancellationToken ct = default)
    {
        object p = string.IsNullOrEmpty(entryId) ? new { } : new { entry_id = entryId };
        await SendRequestAsync("ResetStats", p, ct);
    }

    public async Task<ReverseLookupResultItem> ReverseLookupAsync(string text, string targetMethod, string? shuangpinLayout = null, CancellationToken ct = default)
    {
        var p = new Dictionary<string, object>
        {
            ["text"] = text,
            ["target_method"] = targetMethod
        };
        if (!string.IsNullOrEmpty(shuangpinLayout))
        {
            p["shuangpin_layout"] = ShuangpinLayouts.Canonicalize(shuangpinLayout);
        }

        var data = await SendRequestAsync("ReverseLookup", p, ct);
        return JsonSerializer.Deserialize<ReverseLookupResultItem>(data.GetRawText()) ?? new();
    }

    public async Task<List<PluginModel>> GetPluginsAsync(bool onlyEnabled = false, CancellationToken ct = default)
    {
        var p = new JsonObject { ["only_enabled"] = onlyEnabled };
        var data = await SendRequestAsync("GetPlugins", p, ct);
        if (data.TryGetProperty("plugins", out var pluginsProp))
        {
            return JsonSerializer.Deserialize<List<PluginModel>>(pluginsProp.GetRawText()) ?? [];
        }
        return [];
    }

    public async Task<long> AddPluginAsync(PluginModel plugin, CancellationToken ct = default)
    {
        var p = new
        {
            name = plugin.Name,
            icon = plugin.Icon,
            exec_path = plugin.ExecPath,
            arguments = plugin.Arguments,
            is_enabled = plugin.IsEnabled,
            sort_order = plugin.SortOrder
        };
        var data = await SendRequestAsync("AddPlugin", p, ct);
        if (data.TryGetProperty("id", out var idProp))
        {
            return idProp.GetInt64();
        }
        throw new AdminException(AdminErrorCode.ValidationFailed, "服务端未返回插件 id");
    }

    public async Task UpdatePluginAsync(PluginModel plugin, CancellationToken ct = default)
    {
        var p = new
        {
            id = plugin.Id,
            name = plugin.Name,
            icon = plugin.Icon,
            exec_path = plugin.ExecPath,
            arguments = plugin.Arguments,
            is_enabled = plugin.IsEnabled,
            sort_order = plugin.SortOrder
        };
        await SendRequestAsync("UpdatePlugin", p, ct);
    }

    public async Task DeletePluginAsync(long id, CancellationToken ct = default)
    {
        var p = new { id };
        await SendRequestAsync("DeletePlugin", p, ct);
    }

    public async Task ExecutePluginAsync(long id, CancellationToken ct = default)
    {
        var p = new { id };
        var data = await SendRequestAsync("ExecutePlugin", p, ct);
        if (data.TryGetProperty("success", out var okProp) && okProp.ValueKind == JsonValueKind.False)
        {
            string msg = data.TryGetProperty("message", out var msgProp) ? (msgProp.GetString() ?? "插件执行失败") : "插件执行失败";
            throw new AdminException(AdminErrorCode.ValidationFailed, msg);
        }
    }

    public async Task<List<PluginLogModel>> GetPluginLogsAsync(long? pluginId = null, int limit = 50, CancellationToken ct = default)
    {
        var p = new JsonObject { ["limit"] = limit };
        if (pluginId.HasValue) p["plugin_id"] = pluginId.Value;
        var data = await SendRequestAsync("GetPluginLogs", p, ct);
        if (data.TryGetProperty("logs", out var logsProp))
        {
            return JsonSerializer.Deserialize<List<PluginLogModel>>(logsProp.GetRawText()) ?? [];
        }
        return [];
    }

    public async Task ClearPluginLogsAsync(long? pluginId = null, CancellationToken ct = default)
    {
        var p = new JsonObject();
        if (pluginId.HasValue) p["plugin_id"] = pluginId.Value;
        await SendRequestAsync("ClearPluginLogs", p, ct);
    }

    public async Task ShutdownAsync(CancellationToken ct = default)
    {
        await SendRequestAsync("Shutdown", null, ct);
    }
}
