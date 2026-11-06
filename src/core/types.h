#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ime {

enum class InputScheme : int {
    Unknown = 0,
    Pinyin = 1,
    Wubi86 = 2,
    Custom = 3,
    Shuangpin = 4
};

inline std::string SchemeToString(InputScheme s) {
    switch (s) {
        case InputScheme::Pinyin: return "pinyin";
        case InputScheme::Wubi86: return "wubi";
        case InputScheme::Custom: return "custom";
        case InputScheme::Shuangpin: return "shuangpin";
        default: return "unknown";
    }
}

inline InputScheme SchemeFromString(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower == "pinyin" || lower == "py") return InputScheme::Pinyin;
    if (lower == "wubi" || lower == "wubi86" || lower == "wb") return InputScheme::Wubi86;
    if (lower == "custom" || lower == "phrase") return InputScheme::Custom;
    if (lower == "shuangpin" || lower == "sp") return InputScheme::Shuangpin;
    return InputScheme::Unknown;
}

inline bool IsValidCode(InputScheme scheme, const std::string& code) {
    if (code.empty() || code.length() > 64) return false;
    switch (scheme) {
        case InputScheme::Wubi86: {
            if (code.length() > 4) return false;
            for (char c : code) {
                char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower < 'a' || lower > 'y') return false;
            }
            return true;
        }
        case InputScheme::Pinyin: {
            if (code.front() == '\'' || code.back() == '\'') return false;
            bool prevApos = false;
            for (char c : code) {
                if (c == '\'') {
                    if (prevApos) return false; // no consecutive ''
                    prevApos = true;
                } else {
                    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lower < 'a' || lower > 'z') return false;
                    prevApos = false;
                }
            }
            return true;
        }
        case InputScheme::Custom: {
            for (char c : code) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 33 || uc > 126) return false; // printable ASCII, no whitespace
            }
            return true;
        }
        case InputScheme::Shuangpin: {
            for (char c : code) {
                char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if ((lower < 'a' || lower > 'z') && lower != ';' && lower != '\'') return false;
            }
            return true;
        }
        default:
            return false;
    }
}

inline bool IsValidText(const std::wstring& text) {
    if (text.empty() || text.length() > 512) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<uint16_t>(text[i]);
        if (c == 0) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size()) return false;
            auto low = static_cast<uint16_t>(text[i]);
            if (low < 0xdc00 || low > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}

inline std::string EscapeLike(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() * 2);
    for (char c : input) {
        if (c == '\\' || c == '%' || c == '_') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

enum class MatchQuality : int {
    ExactFull = 0,       // Exact full match with input code
    PrefixFull = 1,      // In multi-syllable pinyin: prefix matches whole syllables
    PrefixCompletion = 2 // Prefix completion (e.g. wubi shorthand or incomplete syllable)
};

inline const char* MatchQualityToString(MatchQuality q) {
    switch (q) {
        case MatchQuality::ExactFull: return "ExactFull";
        case MatchQuality::PrefixFull: return "PrefixFull";
        case MatchQuality::PrefixCompletion: return "PrefixCompletion";
    }
    return "Unknown";
}

enum class EntrySource : int {
    Builtin = 0,
    User = 1
};

struct Candidate {
    std::string entry_id;
    std::wstring text;
    InputScheme scheme = InputScheme::Pinyin;
    std::string matched_code;
    MatchQuality quality = MatchQuality::ExactFull;
    bool is_pinned = false;
    int order_rank = 0;
    double score = 0.0;
    double base_weight = 0.0;
    double effective_count = 0.0;
    int selected_count = 0;
    int64_t last_selected_at = 0;
    EntrySource source = EntrySource::Builtin;
    std::string explain_info;

    // Stage 4 full provenance fields
    std::string dict_id;         // e.g. "dict_custom", "dict_builtin_wubi", "dict_builtin_pinyin"
    std::string layout_id;       // e.g. "natural", "ms", "xiaohe", or ""
    std::string raw_code;        // User typed raw input
    std::string resolved_code;   // Actual resolved code/syllable used for lookup
    int method_priority = 0;     // 0 = primary method, 1, 2... for active input methods order

    // Stage 4C-2 reverse lookup linkage
    std::string reverse_lookup_code;      // Reverse lookup code (e.g. "aaaa" or "gong")
    bool is_reverse_char_by_char = false; // true if derived from char-by-char decomposition
};

struct Entry {
    std::string id;
    std::wstring text;
    EntrySource source = EntrySource::User;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<std::pair<InputScheme, std::string>> codes;
};

struct QueryResult {
    std::string query_code;
    InputScheme scheme = InputScheme::Pinyin;
    std::vector<Candidate> candidates;
};

// UTF-8 / UTF-16 Conversion Helpers
inline std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring wstr(req, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), static_cast<int>(str.length()), &wstr[0], req);
    return wstr;
}

inline std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int req = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr.c_str(), static_cast<int>(wstr.length()), nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string str(req, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr.c_str(), static_cast<int>(wstr.length()), &str[0], req, nullptr, nullptr);
    return str;
}

} // namespace ime

