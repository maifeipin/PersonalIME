#include "shuangpin_decoder.h"
#include <cctype>
#include <algorithm>

namespace ime {

ShuangpinDecoder::ShuangpinDecoder(ShuangpinLayout layout)
    : m_layout(layout) {
}

void ShuangpinDecoder::SetLayout(ShuangpinLayout layout) {
    m_layout = layout;
}

ShuangpinLayout ShuangpinDecoder::GetLayout() const {
    return m_layout;
}

bool ShuangpinDecoder::IsValidInitialKey(char c) const {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (lower >= 'a' && lower <= 'z');
}

bool ShuangpinDecoder::IsValidFinalKey(char c) const {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower >= 'a' && lower <= 'z') return true;
    if (m_layout == ShuangpinLayout::Microsoft && c == ';') return true;
    return false;
}

std::vector<std::string> ShuangpinDecoder::GetInitialsFor(char key) const {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    if (lower == 'v') return {"zh"};
    if (lower == 'i') return {"ch"};
    if (lower == 'u') return {"sh"};
    if (lower >= 'a' && lower <= 'z') return {std::string(1, lower)};
    return {};
}

std::vector<std::string> ShuangpinDecoder::GetFinalsFor(char key) const {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    
    if (m_layout == ShuangpinLayout::Natural) {
        switch (lower) {
            case 'a': return {"a"};
            case 'b': return {"ou"};
            case 'c': return {"iao"};
            case 'd': return {"iang", "uang"};
            case 'e': return {"e"};
            case 'f': return {"en"};
            case 'g': return {"eng"};
            case 'h': return {"ang"};
            case 'i': return {"i"};
            case 'j': return {"an"};
            case 'k': return {"ao"};
            case 'l': return {"ai"};
            case 'm': return {"ian"};
            case 'n': return {"in"};
            case 'o': return {"uo", "o"};
            case 'p': return {"un"};
            case 'q': return {"iu"};
            case 'r': return {"uan", "er"};
            case 's': return {"iong", "ong"};
            case 't': return {"ue", "ve"};
            case 'u': return {"u"};
            case 'v': return {"ui", "v"};
            case 'w': return {"ia", "ua"};
            case 'x': return {"ie"};
            case 'y': return {"ing", "uai"};
            case 'z': return {"ei"};
            default: return {};
        }
    } else if (m_layout == ShuangpinLayout::Microsoft) {
        if (key == ';') return {"ing"};
        switch (lower) {
            case 'a': return {"a"};
            case 'b': return {"ou"};
            case 'c': return {"iao"};
            case 'd': return {"iang", "uang"};
            case 'e': return {"e"};
            case 'f': return {"en"};
            case 'g': return {"eng"};
            case 'h': return {"ang"};
            case 'i': return {"i"};
            case 'j': return {"an"};
            case 'k': return {"ao"};
            case 'l': return {"ai"};
            case 'm': return {"ian"};
            case 'n': return {"in"};
            case 'o': return {"uo", "o"};
            case 'p': return {"un"};
            case 'q': return {"iu"};
            case 'r': return {"uan", "er"};
            case 's': return {"iong", "ong"};
            case 't': return {"ue", "ve"};
            case 'u': return {"u"};
            case 'v': return {"ui", "ve", "v"};
            case 'w': return {"ia", "ua"};
            case 'x': return {"ie"};
            case 'y': return {"uai", "ing"};
            case 'z': return {"ei"};
            default: return {};
        }
    } else if (m_layout == ShuangpinLayout::Xiaohe) {
        switch (lower) {
            case 'a': return {"a"};
            case 'b': return {"in"};
            case 'c': return {"ao"};
            case 'd': return {"ai"};
            case 'e': return {"e"};
            case 'f': return {"en"};
            case 'g': return {"eng"};
            case 'h': return {"ang"};
            case 'i': return {"i"};
            case 'j': return {"an"};
            case 'k': return {"uai", "ing"};
            case 'l': return {"iang", "uang"};
            case 'm': return {"ian"};
            case 'n': return {"iao"};
            case 'o': return {"uo", "o"};
            case 'p': return {"ie"};
            case 'q': return {"iu"};
            case 'r': return {"uan"};
            case 's': return {"iong", "ong"};
            case 't': return {"ue", "ve"};
            case 'u': return {"u"};
            case 'v': return {"ui", "v"};
            case 'w': return {"ei"};
            case 'x': return {"ia", "ua"};
            case 'y': return {"un", "vn"};
            case 'z': return {"ou"};
            default: return {};
        }
    }

    return {};
}

std::vector<std::string> ShuangpinDecoder::DecodeZeroInitialPair(char k1, char k2) const {
    char l1 = static_cast<char>(std::tolower(static_cast<unsigned char>(k1)));
    char l2 = static_cast<char>(std::tolower(static_cast<unsigned char>(k2)));

    if (m_layout == ShuangpinLayout::Microsoft) {
        if (l1 == 'o') {
            switch (l2) {
                case 'a': return {"a"};
                case 'o': return {"o"};
                case 'e': return {"e"};
                case 'l': return {"ai"};
                case 'z': return {"ei"};
                case 'k': return {"ao"};
                case 'b': return {"ou"};
                case 'j': return {"an"};
                case 'f': return {"en"};
                case 'h': return {"ang"};
                case 'g': return {"eng"};
                case 'r': return {"er"};
                default: break;
            }
        }
        return {};
    }

    // Natural Code & Xiaohe:
    // Single-vowel syllables: aa -> a, oo -> o, ee -> e
    if (l1 == 'a' && l2 == 'a') return {"a"};
    if (l1 == 'o' && l2 == 'o') return {"o"};
    if (l1 == 'e' && l2 == 'e') return {"e"};

    // Two-vowel syllables directly typed: ai, an, ao, ou, en, ei, er
    if (l1 == 'a' && l2 == 'i') return {"ai"};
    if (l1 == 'a' && l2 == 'n') return {"an"};
    if (l1 == 'a' && l2 == 'o') return {"ao"};
    if (l1 == 'o' && l2 == 'u') return {"ou"};
    if (l1 == 'e' && l2 == 'n') return {"en"};
    if (l1 == 'e' && l2 == 'i') return {"ei"};
    if (l1 == 'e' && l2 == 'r') return {"er"};

    // Three-letter zero-initials: first letter + final key
    // ang -> ah (h is ang)
    // eng -> eg (g is eng)
    if (l1 == 'a' && l2 == 'h') return {"ang"};
    if (l1 == 'e' && l2 == 'g') return {"eng"};

    return {};
}

std::vector<std::string> ShuangpinDecoder::DecodePair(char initialKey, char finalKey) const {
    std::vector<std::string> results;

    // 1. Check if this is a valid zero-initial pair
    std::vector<std::string> zeroInit = DecodeZeroInitialPair(initialKey, finalKey);
    for (const auto& z : zeroInit) {
        if (m_segmenter.IsValidSyllable(z)) {
            if (std::find(results.begin(), results.end(), z) == results.end()) {
                results.push_back(z);
            }
        }
    }

    // 2. Standard consonant initial + final
    std::vector<std::string> inits = GetInitialsFor(initialKey);
    std::vector<std::string> finals = GetFinalsFor(finalKey);

    for (const auto& init : inits) {
        for (const auto& fin : finals) {
            std::vector<std::string> candidates;
            candidates.push_back(init + fin);

            // Handle ü spelling rules
            if (init == "j" || init == "q" || init == "x" || init == "y") {
                if (fin == "v") candidates.push_back(init + "u");
                if (fin == "ve") candidates.push_back(init + "ue");
                if (fin == "van") candidates.push_back(init + "uan");
                if (fin == "vn") candidates.push_back(init + "un");
            } else if (init == "n" || init == "l") {
                if (fin == "v") {
                    candidates.push_back(init + "v");
                }
                if (fin == "ve") {
                    candidates.push_back(init + "ve");
                    candidates.push_back(init + "ue");
                }
            }

            for (const auto& cand : candidates) {
                if (m_segmenter.IsValidSyllable(cand)) {
                    if (std::find(results.begin(), results.end(), cand) == results.end()) {
                        results.push_back(cand);
                    }
                }
            }
        }
    }

    return results;
}

#include <unordered_set>

static void GenerateBranchesRecursive(
    const std::vector<std::vector<std::string>>& options,
    size_t step,
    std::vector<std::string>& currentSyllables,
    const std::string& trailingInit,
    std::unordered_set<std::string>& seenContinuous,
    std::vector<ShuangpinBranch>& outBranches,
    bool& wasTruncated) {
    if (wasTruncated) {
        return;
    }
    if (step == options.size()) {
        std::string cont;
        std::string sp;
        for (size_t i = 0; i < currentSyllables.size(); ++i) {
            if (i > 0) sp += " ";
            sp += currentSyllables[i];
            cont += currentSyllables[i];
        }
        cont += trailingInit;
        if (!cont.empty() && seenContinuous.find(cont) == seenContinuous.end()) {
            if (outBranches.size() < MAX_SHUANGPIN_BRANCHES) {
                seenContinuous.insert(cont);
                ShuangpinBranch b;
                b.syllables = currentSyllables;
                b.trailing_initial = trailingInit;
                b.continuous = cont;
                b.spaced = sp;
                outBranches.push_back(std::move(b));
            } else {
                wasTruncated = true;
            }
        }
        return;
    }

    for (const auto& syl : options[step]) {
        currentSyllables.push_back(syl);
        GenerateBranchesRecursive(options, step + 1, currentSyllables, trailingInit, seenContinuous, outBranches, wasTruncated);
        currentSyllables.pop_back();
        if (wasTruncated) {
            return;
        }
    }
}

ShuangpinDecodeResult ShuangpinDecoder::Decode(const std::string& raw) const {
    ShuangpinDecodeResult res;
    res.raw_input = raw;
    if (raw.empty()) {
        res.is_valid = false;
        return res;
    }

    // Input length limit
    if (raw.length() > MAX_SHUANGPIN_INPUT_LENGTH) {
        res.is_valid = false;
        res.error_detail = "Shuangpin input exceeds maximum supported length";
        return res;
    }

    // 1. Strict character validation
    // Allowed: a-z, A-Z, '\''
    // If Microsoft layout: also ';' is allowed
    for (char c : raw) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool ok = (lower >= 'a' && lower <= 'z') || (c == '\'');
        if (m_layout == ShuangpinLayout::Microsoft && c == ';') {
            ok = true;
        }
        if (!ok) {
            res.is_valid = false;
            res.error_detail = "Illegal character in shuangpin input: " + std::string(1, c);
            return res;
        }
    }

    // 2. Validate syllable separator apostrophe '\''
    if (raw.front() == '\'') {
        res.is_valid = false;
        res.error_detail = "Leading apostrophe is invalid";
        return res;
    }

    std::string clean;
    std::vector<size_t> chunkLengths;
    size_t curChunk = 0;
    bool prevApos = false;

    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\'') {
            if (prevApos) {
                res.is_valid = false;
                res.error_detail = "Consecutive apostrophes are invalid";
                return res;
            }
            if (curChunk == 0) {
                res.is_valid = false;
                res.error_detail = "Empty chunk before apostrophe";
                return res;
            }
            // A syllable separator can only appear after complete 2-char syllables!
            if (curChunk % 2 != 0) {
                res.is_valid = false;
                res.error_detail = "Apostrophe placed after odd length chunk: " + std::to_string(curChunk);
                return res;
            }
            chunkLengths.push_back(curChunk);
            curChunk = 0;
            prevApos = true;
        } else {
            clean.push_back(c);
            curChunk++;
            prevApos = false;
        }
    }
    if (curChunk > 0) {
        chunkLengths.push_back(curChunk);
    }

    // 3. Now decode pair by pair
    size_t idx = 0;
    std::vector<std::string> decodedSyllables;
    std::vector<std::vector<std::string>> allPairOptions;
    std::string trailing;

    for (size_t chunkIdx = 0; chunkIdx < chunkLengths.size(); ++chunkIdx) {
        size_t chunkLen = chunkLengths[chunkIdx];
        size_t chunkEnd = idx + chunkLen;
        while (idx + 1 < chunkEnd) {
            char k1 = clean[idx];
            char k2 = clean[idx + 1];
            auto pairs = DecodePair(k1, k2);
            if (pairs.empty()) {
                res.is_valid = false;
                res.error_detail = "Invalid shuangpin pair: " + std::string(1, k1) + std::string(1, k2);
                return res;
            }
            decodedSyllables.push_back(pairs.front());
            allPairOptions.push_back(pairs);
            idx += 2;
        }

        if (idx < chunkEnd) {
            // Trailing odd character must be in the final chunk and must be a valid initial key
            char trailChar = clean[idx];
            if (!IsValidInitialKey(trailChar)) {
                res.is_valid = false;
                res.error_detail = "Invalid trailing initial key: " + std::string(1, trailChar);
                return res;
            }
            trailing.push_back(trailChar);
            idx++;
        }
    }

    std::string trailingInit;
    if (!trailing.empty()) {
        char t = trailing.front();
        auto inits = GetInitialsFor(t);
        if (!inits.empty()) {
            trailingInit = inits.front();
        } else {
            trailingInit = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(t))));
        }
    }

    res.is_valid = true;
    res.is_complete = trailing.empty();
    res.syllables = decodedSyllables;
    res.trailing_incomplete = trailing;
    res.trailing_initial = trailingInit;

    // Build continuous / spaced pinyin strings for primary branch
    for (size_t i = 0; i < decodedSyllables.size(); ++i) {
        if (i > 0) res.pinyin_spaced += " ";
        res.pinyin_spaced += decodedSyllables[i];
        res.pinyin_continuous += decodedSyllables[i];
    }
    if (!trailingInit.empty()) {
        res.pinyin_continuous += trailingInit;
    }

    // Generate bounded branches with deduplication and early pruning (P1-4)
    std::unordered_set<std::string> seenContinuous;
    std::vector<std::string> currentSyllables;
    bool wasTruncated = false;
    GenerateBranchesRecursive(allPairOptions, 0, currentSyllables, trailingInit, seenContinuous, res.branches, wasTruncated);
    res.is_truncated = wasTruncated;

    // If branches are empty (e.g. single trailing char), add single branch
    if (res.branches.empty() && !res.pinyin_continuous.empty()) {
        ShuangpinBranch b;
        b.syllables = decodedSyllables;
        b.trailing_initial = trailingInit;
        b.continuous = res.pinyin_continuous;
        b.spaced = res.pinyin_spaced;
        res.branches.push_back(std::move(b));
    }

    for (const auto& b : res.branches) {
        res.alternative_pinyins.push_back(b.continuous);
    }

    return res;
}

std::string ShuangpinDecoder::EncodePinyinSyllable(const std::string& rawSyllable) const {
    if (rawSyllable.empty()) return "";
    std::string syl = rawSyllable;
    std::transform(syl.begin(), syl.end(), syl.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (!m_segmenter.IsValidSyllable(syl)) {
        return "";
    }

    // 1. Zero initial syllables
    if (m_layout == ShuangpinLayout::Microsoft) {
        if (syl == "a") return "oa";
        if (syl == "o") return "oo";
        if (syl == "e") return "oe";
        if (syl == "ai") return "ol";
        if (syl == "ei") return "oz";
        if (syl == "ao") return "ok";
        if (syl == "ou") return "ob";
        if (syl == "an") return "oj";
        if (syl == "en") return "of";
        if (syl == "ang") return "oh";
        if (syl == "eng") return "og";
        if (syl == "er") return "or";
    } else { // Natural & Xiaohe
        if (syl == "a") return "aa";
        if (syl == "o") return "oo";
        if (syl == "e") return "ee";
        if (syl == "ai") return "ai";
        if (syl == "ei") return "ei";
        if (syl == "ao") return "ao";
        if (syl == "ou") return "ou";
        if (syl == "an") return "an";
        if (syl == "en") return "en";
        if (syl == "er") return "er";
        if (syl == "ang") return "ah";
        if (syl == "eng") return "eg";
    }

    // 2. Syllables with consonant initial
    char initialKey = '\0';
    std::string fin;

    if (syl.rfind("zh", 0) == 0) {
        initialKey = 'v';
        fin = syl.substr(2);
    } else if (syl.rfind("ch", 0) == 0) {
        initialKey = 'i';
        fin = syl.substr(2);
    } else if (syl.rfind("sh", 0) == 0) {
        initialKey = 'u';
        fin = syl.substr(2);
    } else {
        initialKey = syl[0];
        fin = syl.substr(1);
    }

    char finalKey = '\0';
    if (m_layout == ShuangpinLayout::Natural) {
        if (fin == "a") finalKey = 'a';
        else if (fin == "ou") finalKey = 'b';
        else if (fin == "iao") finalKey = 'c';
        else if (fin == "iang" || fin == "uang") finalKey = 'd';
        else if (fin == "e") finalKey = 'e';
        else if (fin == "en") finalKey = 'f';
        else if (fin == "eng") finalKey = 'g';
        else if (fin == "ang") finalKey = 'h';
        else if (fin == "i") finalKey = 'i';
        else if (fin == "an") finalKey = 'j';
        else if (fin == "ao") finalKey = 'k';
        else if (fin == "ai") finalKey = 'l';
        else if (fin == "ian") finalKey = 'm';
        else if (fin == "in") finalKey = 'n';
        else if (fin == "uo" || fin == "o") finalKey = 'o';
        else if (fin == "un") finalKey = 'p';
        else if (fin == "iu") finalKey = 'q';
        else if (fin == "uan" || fin == "er") finalKey = 'r';
        else if (fin == "iong" || fin == "ong") finalKey = 's';
        else if (fin == "ue" || fin == "ve") finalKey = 't';
        else if (fin == "u") finalKey = 'u';
        else if (fin == "ui" || fin == "v") finalKey = 'v';
        else if (fin == "ia" || fin == "ua") finalKey = 'w';
        else if (fin == "ie") finalKey = 'x';
        else if (fin == "ing" || fin == "uai") finalKey = 'y';
        else if (fin == "ei") finalKey = 'z';
    } else if (m_layout == ShuangpinLayout::Microsoft) {
        if (fin == "ing") finalKey = ';';
        else if (fin == "a") finalKey = 'a';
        else if (fin == "ou") finalKey = 'b';
        else if (fin == "iao") finalKey = 'c';
        else if (fin == "iang" || fin == "uang") finalKey = 'd';
        else if (fin == "e") finalKey = 'e';
        else if (fin == "en") finalKey = 'f';
        else if (fin == "eng") finalKey = 'g';
        else if (fin == "ang") finalKey = 'h';
        else if (fin == "i") finalKey = 'i';
        else if (fin == "an") finalKey = 'j';
        else if (fin == "ao") finalKey = 'k';
        else if (fin == "ai") finalKey = 'l';
        else if (fin == "ian") finalKey = 'm';
        else if (fin == "in") finalKey = 'n';
        else if (fin == "uo" || fin == "o") finalKey = 'o';
        else if (fin == "un") finalKey = 'p';
        else if (fin == "iu") finalKey = 'q';
        else if (fin == "uan" || fin == "er") finalKey = 'r';
        else if (fin == "iong" || fin == "ong") finalKey = 's';
        else if (fin == "ue" || fin == "ve") finalKey = 't';
        else if (fin == "u") finalKey = 'u';
        else if (fin == "ui" || fin == "v") finalKey = 'v';
        else if (fin == "ia" || fin == "ua") finalKey = 'w';
        else if (fin == "ie") finalKey = 'x';
        else if (fin == "uai") finalKey = 'y';
        else if (fin == "ei") finalKey = 'z';
    } else if (m_layout == ShuangpinLayout::Xiaohe) {
        if (fin == "a") finalKey = 'a';
        else if (fin == "in") finalKey = 'b';
        else if (fin == "ao") finalKey = 'c';
        else if (fin == "ai") finalKey = 'd';
        else if (fin == "e") finalKey = 'e';
        else if (fin == "en") finalKey = 'f';
        else if (fin == "eng") finalKey = 'g';
        else if (fin == "ang") finalKey = 'h';
        else if (fin == "i") finalKey = 'i';
        else if (fin == "an") finalKey = 'j';
        else if (fin == "uai" || fin == "ing") finalKey = 'k';
        else if (fin == "iang" || fin == "uang") finalKey = 'l';
        else if (fin == "ian") finalKey = 'm';
        else if (fin == "iao") finalKey = 'n';
        else if (fin == "uo" || fin == "o") finalKey = 'o';
        else if (fin == "ie") finalKey = 'p';
        else if (fin == "iu") finalKey = 'q';
        else if (fin == "uan") finalKey = 'r';
        else if (fin == "iong" || fin == "ong") finalKey = 's';
        else if (fin == "ue" || fin == "ve") finalKey = 't';
        else if (fin == "u") finalKey = 'u';
        else if (fin == "ui" || fin == "v") finalKey = 'v';
        else if (fin == "ei") finalKey = 'w';
        else if (fin == "ia" || fin == "ua") finalKey = 'x';
        else if (fin == "un" || fin == "vn") finalKey = 'y';
        else if (fin == "ou") finalKey = 'z';
    }

    if (finalKey == '\0') {
        return "";
    }

    std::string res;
    res.push_back(initialKey);
    res.push_back(finalKey);
    return res;
}

std::string ShuangpinDecoder::EncodePinyin(const std::vector<std::string>& syllables, bool spaced) const {
    if (syllables.empty()) return "";
    std::string result;
    for (size_t i = 0; i < syllables.size(); ++i) {
        std::string code = EncodePinyinSyllable(syllables[i]);
        if (code.empty()) return "";
        if (spaced && i > 0) result.push_back(' ');
        result += code;
    }
    return result;
}

} // namespace ime

