#pragma once

#include <string>
#include <vector>
#include <unordered_set>

namespace ime {

struct PinyinSegmentation {
    std::vector<std::string> syllables;
    std::string raw_continuous;     // e.g. "xian"
    std::string spaced_syllables;   // e.g. "xi an"
    bool has_explicit_separator = false;
};

class PinyinSegmenter {
public:
    PinyinSegmenter();

    // Segment an input string (e.g. "xian", "xi'an", "zhongwen") into all valid syllable sequences
    std::vector<PinyinSegmentation> Segment(const std::string& input) const;

    bool IsValidSyllable(const std::string& syl) const;

private:
    std::unordered_set<std::string> m_validSyllables;

    void Backtrack(const std::string& s, size_t start,
                   std::vector<std::string>& current,
                   std::vector<std::vector<std::string>>& results) const;
};

} // namespace ime

