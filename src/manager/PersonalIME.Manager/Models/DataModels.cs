using System.Text.Json;
using System.Text.Json.Serialization;

namespace PersonalIME.Manager.Models;

public static class InputSchemes
{
    public const string Wubi86 = "wubi86";
    public const string Pinyin = "pinyin";
    public const string Shuangpin = "shuangpin";
    public const string Custom = "custom";

    public static string ToDisplayName(string scheme) => scheme switch
    {
        Wubi86 => "五笔86",
        Pinyin => "全拼",
        Shuangpin => "双拼",
        Custom => "自定义",
        _ => scheme
    };
}

public static class ShuangpinLayouts
{
    public const string Natural = "natural";
    public const string Microsoft = "ms";
    public const string Xiaohe = "xiaohe";

    public static readonly string[] All = [Natural, Microsoft, Xiaohe];

    public static string ToDisplayName(string layout) => layout switch
    {
        Natural => "自然码",
        Microsoft => "微软双拼",
        Xiaohe => "小鹤双拼",
        _ => layout
    };

    public static string Canonicalize(string? layout) => (layout?.Trim().ToLowerInvariant()) switch
    {
        "ziranma" => Natural,
        "natural" => Natural,
        "weiruan" => Microsoft,
        "ms" => Microsoft,
        "microsoft" => Microsoft,
        "xiaohe" => Xiaohe,
        "flypy" => Xiaohe,
        _ => layout ?? Natural
    };
}

public class EntryCodeItem
{
    [JsonPropertyName("scheme")]
    public string Scheme { get; set; } = InputSchemes.Wubi86;

    [JsonPropertyName("code")]
    public string Code { get; set; } = string.Empty;

    public string SchemeDisplayName => InputSchemes.ToDisplayName(Scheme);
}

public class UserEntryItem
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("source")]
    public int Source { get; set; } = 1;

    [JsonPropertyName("codes")]
    public List<EntryCodeItem> Codes { get; set; } = [];

    public string FormattedCodes => string.Join(", ", Codes.Select(c => $"{c.SchemeDisplayName}: {c.Code}"));
    public string SummaryText => Text.Length > 30 ? Text[..30] + "..." : Text;

    public bool IsSystem => Source == 0 || Id.StartsWith("builtin:");
    public string SourceDisplayName => IsSystem ? "系统内置" : "用户词库";
}

public class PinnedItem
{
    [JsonPropertyName("entry_id")]
    public string EntryId { get; set; } = string.Empty;

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("order_rank")]
    public int OrderRank { get; set; }

    public string Scheme { get; set; } = string.Empty;
    public string Code { get; set; } = string.Empty;
    public string? LayoutId { get; set; }
}

public class DictionaryPriorityItem
{
    private string _dictId = string.Empty;

    [JsonPropertyName("dict_id")]
    public string DictId
    {
        get => _dictId;
        set => _dictId = value;
    }

    // For compatibility with tests or callers using Id
    [JsonIgnore]
    public string Id
    {
        get => _dictId;
        set => _dictId = value;
    }

    [JsonIgnore]
    public string Name => GetFriendlyName(_dictId);

    [JsonPropertyName("enabled")]
    public bool IsEnabled { get; set; } = true;

    public static string GetFriendlyName(string id) => id switch
    {
        "dict_custom" => "用户自定义词库",
        "dict_user" => "用户自定义词库",
        "dict_builtin_wubi" => "五笔86内置词库",
        "dict_builtin_pinyin" => "全拼内置词库",
        _ => id
    };
}

public class InputMethodPriorityItem
{
    public string Scheme { get; set; } = string.Empty;
    public bool IsEnabled { get; set; } = true;
    public string DisplayName => InputSchemes.ToDisplayName(Scheme);
}

public class InputMethodConfigItem
{
    [JsonPropertyName("scheme")]
    public string Scheme { get; set; } = string.Empty;

    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; } = true;
}

public class InputPolicyConfig
{
    [JsonPropertyName("revision")]
    public uint Revision { get; set; }

    [JsonPropertyName("candidate_page_size")]
    public int CandidatePageSize { get; set; } = 5;

    [JsonPropertyName("fallback_candidate_threshold")]
    public int FallbackCandidateThreshold { get; set; } = 3;

    [JsonPropertyName("primary_input_method")]
    public string PrimaryInputMethod { get; set; } = InputSchemes.Wubi86;

    [JsonPropertyName("shuangpin_layout")]
    public string ShuangpinLayout { get; set; } = ShuangpinLayouts.Xiaohe;

    [JsonPropertyName("frequency_enabled")]
    public bool FrequencyEnabled { get; set; } = true;

    [JsonPropertyName("learning_enabled")]
    public bool LearningEnabled { get; set; } = true;

    [JsonPropertyName("mixed_input_enabled")]
    public bool MixedInputEnabled { get; set; } = true;

    [JsonPropertyName("initial_mode")]
    public InitialModeConfig InitialMode { get; set; } = new();

    [JsonPropertyName("reverse_lookup")]
    public ReverseLookupConfig ReverseLookup { get; set; } = new();

    [JsonPropertyName("input_method_order")]
    public List<string> MethodPriorities { get; set; } = [
        InputSchemes.Wubi86,
        InputSchemes.Pinyin,
        InputSchemes.Shuangpin
    ];

    [JsonPropertyName("enabled_input_methods")]
    public List<string>? EnabledInputMethods { get; set; } = [
        InputSchemes.Wubi86,
        InputSchemes.Pinyin,
        InputSchemes.Shuangpin
    ];

    [JsonPropertyName("input_methods")]
    public List<InputMethodConfigItem>? InputMethods { get; set; } = [
        new() { Scheme = InputSchemes.Wubi86, Enabled = true },
        new() { Scheme = InputSchemes.Pinyin, Enabled = true },
        new() { Scheme = InputSchemes.Shuangpin, Enabled = true }
    ];

    [JsonPropertyName("dictionary_order")]
    public List<DictionaryPriorityItem> DictionaryPriorities { get; set; } = [
        new() { DictId = "dict_custom", IsEnabled = true },
        new() { DictId = "dict_builtin_wubi", IsEnabled = true },
        new() { DictId = "dict_builtin_pinyin", IsEnabled = true }
    ];
}

public class InitialModeConfig
{
    [JsonPropertyName("chinese")]
    public bool Chinese { get; set; } = true;

    [JsonPropertyName("full_width")]
    public bool FullWidth { get; set; } = false;

    [JsonPropertyName("chinese_punctuation")]
    public bool ChinesePunctuation { get; set; } = true;

    [JsonIgnore]
    public bool FullPunct
    {
        get => ChinesePunctuation;
        set => ChinesePunctuation = value;
    }
}

public class ReverseLookupConfig
{
    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; } = false;

    [JsonPropertyName("target_method")]
    public string TargetMethod { get; set; } = InputSchemes.Pinyin;

    [JsonPropertyName("shuangpin_layout")]
    public string ShuangpinLayout { get; set; } = ShuangpinLayouts.Natural;

    [JsonPropertyName("timing")]
    public string Timing { get; set; } = "always";
}

public class ReverseLookupResultItem
{
    [JsonPropertyName("found")]
    public bool Found { get; set; }

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("target_method")]
    public string TargetMethod { get; set; } = string.Empty;

    [JsonPropertyName("is_phrase")]
    public bool IsPhrase { get; set; }

    [JsonPropertyName("code")]
    public string Code { get; set; } = string.Empty;

    [JsonPropertyName("primary_code")]
    public string PrimaryCode { get; set; } = string.Empty;

    [JsonPropertyName("all_codes")]
    public List<string> AllCodes { get; set; } = [];

    [JsonPropertyName("char_codes")]
    public List<string> CharCodes { get; set; } = [];

    public string DisplayCode => !string.IsNullOrEmpty(PrimaryCode) ? PrimaryCode : Code;
    public string ModeDescription => IsPhrase ? "整词编码" : "逐字拆码";
}

public class ServiceStatusInfo
{
    [JsonPropertyName("service_version")]
    public string ServiceVersion { get; set; } = "1.0.0";

    [JsonPropertyName("uptime_seconds")]
    public long UptimeSeconds { get; set; }

    [JsonPropertyName("active_clients")]
    public int ActiveClients { get; set; }

    [JsonPropertyName("db_path")]
    public string DatabasePath { get; set; } = string.Empty;

    [JsonPropertyName("db_size_bytes")]
    public long DatabaseSizeBytes { get; set; }

    [JsonPropertyName("last_commit_receipt_time")]
    public string LastCommitReceiptTime { get; set; } = string.Empty;
}

public class ScopeCandidateItem
{
    [JsonPropertyName("entry_id")]
    public string EntryId { get; set; } = string.Empty;

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("matched_code")]
    public string MatchedCode { get; set; } = string.Empty;

    [JsonPropertyName("raw_code")]
    public string RawCode { get; set; } = string.Empty;

    [JsonPropertyName("dict_id")]
    public string DictId { get; set; } = string.Empty;
}

public class GetEntriesResult
{
    [JsonPropertyName("total")]
    public int Total { get; set; }

    [JsonPropertyName("page")]
    public int Page { get; set; }

    [JsonPropertyName("page_size")]
    public int PageSize { get; set; }

    [JsonPropertyName("entries")]
    public List<UserEntryItem> Entries { get; set; } = [];
}

public class GetPinnedResult
{
    [JsonPropertyName("scheme")]
    public string Scheme { get; set; } = string.Empty;

    [JsonPropertyName("code")]
    public string Code { get; set; } = string.Empty;

    [JsonPropertyName("layout_id")]
    public string? LayoutId { get; set; }

    [JsonPropertyName("revision")]
    public ulong Revision { get; set; }

    [JsonPropertyName("entries")]
    public List<PinnedItem> Entries { get; set; } = [];
}

public sealed class FlexibleStringOrTimestampConverter : JsonConverter<string>
{
    public override string Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType == JsonTokenType.String)
        {
            return reader.GetString() ?? string.Empty;
        }
        if (reader.TokenType == JsonTokenType.Number && reader.TryGetInt64(out long unixSeconds))
        {
            if (unixSeconds <= 0) return string.Empty;
            try
            {
                return DateTimeOffset.FromUnixTimeSeconds(unixSeconds).ToLocalTime().ToString("HH:mm:ss");
            }
            catch
            {
                return unixSeconds.ToString();
            }
        }
        return string.Empty;
    }

    public override void Write(Utf8JsonWriter writer, string value, JsonSerializerOptions options)
    {
        writer.WriteStringValue(value ?? string.Empty);
    }
}

public class PluginModel
{
    [JsonPropertyName("id")]
    public long Id { get; set; }

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("icon")]
    public string Icon { get; set; } = "🧩";

    [JsonPropertyName("exec_path")]
    public string ExecPath { get; set; } = string.Empty;

    [JsonPropertyName("arguments")]
    public string Arguments { get; set; } = string.Empty;

    [JsonPropertyName("is_enabled")]
    public bool IsEnabled { get; set; } = true;

    [JsonPropertyName("sort_order")]
    public int SortOrder { get; set; }

    [JsonPropertyName("created_at")]
    [JsonConverter(typeof(FlexibleStringOrTimestampConverter))]
    public string CreatedAt { get; set; } = string.Empty;
}

public class PluginLogModel
{
    [JsonPropertyName("id")]
    public long Id { get; set; }

    [JsonPropertyName("plugin_id")]
    public long PluginId { get; set; }

    [JsonPropertyName("plugin_name")]
    public string PluginName { get; set; } = string.Empty;

    [JsonPropertyName("command_line")]
    public string CommandLine { get; set; } = string.Empty;

    [JsonPropertyName("exec_path")]
    public string ExecPath { get; set; } = string.Empty;

    [JsonPropertyName("arguments")]
    public string Arguments { get; set; } = string.Empty;

    [JsonPropertyName("status")]
    public string Status { get; set; } = string.Empty;

    [JsonPropertyName("message")]
    public string Message
    {
        get => ErrorMessage;
        set => ErrorMessage = value;
    }

    [JsonPropertyName("error_message")]
    public string ErrorMessage { get; set; } = string.Empty;

    [JsonPropertyName("executed_at")]
    [JsonConverter(typeof(FlexibleStringOrTimestampConverter))]
    public string ExecutedAt { get; set; } = string.Empty;
}

