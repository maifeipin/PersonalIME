#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "pinyin_segmenter.h"

namespace ime {

enum class ShuangpinLayout : int {
    Natural = 0,   // 自然码 (Zrm 2000)
    Microsoft = 1, // 微软双拼 (Windows MS Pinyin)
    Xiaohe = 2     // 小鹤双拼 (Flypy)
};

inline std::string ShuangpinLayoutToString(ShuangpinLayout layout) {
    switch (layout) {
        case ShuangpinLayout::Natural: return "natural";
        case ShuangpinLayout::Microsoft: return "ms";
        case ShuangpinLayout::Xiaohe: return "xiaohe";
        default: return "natural";
    }
}

inline ShuangpinLayout ShuangpinLayoutFromString(const std::string& str) {
    if (str == "ms" || str == "microsoft") return ShuangpinLayout::Microsoft;
    if (str == "xiaohe" || str == "flypy") return ShuangpinLayout::Xiaohe;
    return ShuangpinLayout::Natural;
}

inline constexpr size_t MAX_SHUANGPIN_BRANCHES = 16;
inline constexpr size_t MAX_SHUANGPIN_INPUT_LENGTH = 64;

struct ShuangpinBranch {
    std::vector<std::string> syllables;
    std::string trailing_initial;
    std::string continuous;
    std::string spaced;
};

struct ShuangpinDecodeResult {
    bool is_valid = false;
    bool is_complete = false;             // true if full sequence decoded without leftover odd char
    bool is_truncated = false;            // true if ambiguous paths exceeded budget and were truncated
    std::string raw_input;                // e.g. "vswf"
    std::vector<std::string> syllables;   // e.g. ["zhong", "wen"]
    std::string pinyin_continuous;        // e.g. "zhongwen"
    std::string pinyin_spaced;            // e.g. "zhong wen"
    std::string trailing_incomplete;      // e.g. "w" if odd length
    std::string trailing_initial;         // resolved initial string for trailing key, e.g. "w" or "zh"
    std::vector<std::string> alternative_pinyins; // All possible disambiguated full pinyin strings (bounded)
    std::vector<ShuangpinBranch> branches;        // Structured branches with explicit boundaries
    std::string error_detail;
};

class ShuangpinDecoder {
public:
    explicit ShuangpinDecoder(ShuangpinLayout layout = ShuangpinLayout::Natural);

    void SetLayout(ShuangpinLayout layout);
    ShuangpinLayout GetLayout() const;

    // Decode user typed raw key sequence (supports single quotes for syllable separation)
    ShuangpinDecodeResult Decode(const std::string& raw) const;

    // Encode a single Pinyin syllable into a 2-char Shuangpin code (returns "" if invalid)
    std::string EncodePinyinSyllable(const std::string& syllable) const;

    // Encode a sequence of Pinyin syllables into Shuangpin (empty string if any syllable fails)
    std::string EncodePinyin(const std::vector<std::string>& syllables, bool spaced = false) const;

    // Decode a single 2-char pair into a valid Pinyin syllable
    // Disambiguates combined keys (e.g. iang/uang) using standard Mandarin phonotactics
    std::vector<std::string> DecodePair(char initialKey, char finalKey) const;

    // Validate if character can serve as initial key
    bool IsValidInitialKey(char c) const;

    // Validate if character can serve as final key
    bool IsValidFinalKey(char c) const;

    std::vector<std::string> GetInitialsFor(char key) const;
    std::vector<std::string> GetFinalsFor(char key) const;

private:
    ShuangpinLayout m_layout;
    PinyinSegmenter m_segmenter;

    std::vector<std::string> DecodeZeroInitialPair(char k1, char k2) const;
};

} // namespace ime

