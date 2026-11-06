#pragma once

#include "../types.h"
#include "../encoding/pinyin_segmenter.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include <optional>

namespace ime {

class ShuangpinDecoder;

struct BuiltinDictItem {
    std::string id;
    std::wstring text;
    std::string code;
    double weight = 0.0;
};

struct BuiltinPinyinItem {
    std::string id;
    std::wstring text;
    std::string continuous_code; // e.g. "xian", "zhongwen"
    std::string spaced_syllables; // e.g. "xi an", "zhong wen"
    double weight = 0.0;
};

class BuiltinDictionary {
public:
    BuiltinDictionary();

    bool Load(const std::string& dictDir);
    bool LoadWubi(const std::string& filePath);
    bool LoadPinyin(const std::string& filePath);

    bool IsLoaded() const { return m_loaded; }
    size_t WubiEntryCount() const { return m_wubiCount; }
    size_t PinyinEntryCount() const { return m_pinyinCount; }

    bool HasItem(const std::string& id) const;
    std::optional<std::wstring> GetTextById(const std::string& id) const;
    std::optional<Candidate> GetCandidateById(InputScheme scheme, const std::string& code, const std::string& id) const;

    std::vector<Candidate> QueryWubi(const std::string& code) const;
    std::vector<Candidate> QueryPinyin(const std::string& code, const PinyinSegmenter& segmenter) const;
    std::vector<Candidate> QueryPinyinExplicit(
        const std::vector<std::string>& syllables,
        const std::string& trailingInitial,
        bool isComplete) const;

    bool CheckPinyinItemMatchesSyllables(
        const std::string& id,
        const std::vector<std::string>& syllables,
        const std::string& trailingInitial,
        bool isComplete) const;

    // Stage 4C-2 text-to-code reverse lookup
    std::vector<std::string> ReverseLookupWubi(const std::wstring& text) const;
    std::vector<std::string> ReverseLookupPinyin(const std::wstring& text) const;
    std::vector<std::string> ReverseLookupShuangpin(const std::wstring& text, const ShuangpinDecoder& decoder) const;

private:
    bool m_loaded = false;
    size_t m_wubiCount = 0;
    size_t m_pinyinCount = 0;

    // Wubi index: exact & prefix
    std::unordered_map<std::string, std::vector<BuiltinDictItem>> m_wubiExact;
    std::vector<std::pair<std::string, BuiltinDictItem>> m_wubiSortedAll; // sorted by code for fast std::lower_bound
    std::unordered_map<std::string, BuiltinDictItem> m_wubiById;
    std::unordered_map<std::wstring, std::vector<BuiltinDictItem>> m_wubiByText;

    // Pinyin index: exact & prefix
    std::unordered_map<std::string, std::vector<BuiltinPinyinItem>> m_pinyinExactContinuous;
    std::unordered_map<std::string, std::vector<BuiltinPinyinItem>> m_pinyinExactSpaced;
    std::vector<std::pair<std::string, BuiltinPinyinItem>> m_pinyinSortedContinuous; // sorted by continuous_code
    std::unordered_map<std::string, BuiltinPinyinItem> m_pinyinById;
    std::unordered_map<std::wstring, std::vector<BuiltinPinyinItem>> m_pinyinByText;
};

} // namespace ime

