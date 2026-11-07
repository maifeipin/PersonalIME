#include "builtin_dict.h"
#include "../encoding/shuangpin_decoder.h"
#include "../encoding/wubi_encoder.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <optional>
#include <unordered_set>

namespace ime {

BuiltinDictionary::BuiltinDictionary() {}

bool BuiltinDictionary::Load(const std::string& dictDir) {
    std::string wubiPath = dictDir + "/wubi86.dict";
    std::string pinyinPath = dictDir + "/pinyin.dict";

    bool ok1 = LoadWubi(wubiPath);
    bool ok2 = LoadPinyin(pinyinPath);
    m_loaded = (ok1 || ok2);
    return m_loaded;
}

bool BuiltinDictionary::LoadWubi(const std::string& filePath) {
    std::ifstream f(filePath);
    if (!f.is_open()) return false;

    m_wubiExact.clear();
    m_wubiSortedAll.clear();
    m_wubiById.clear();
    m_wubiByText.clear();
    m_wubiCount = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string textStr, code, weightStr;
        if (std::getline(ss, textStr, '\t') && std::getline(ss, code, '\t')) {
            code = WubiEncoder::Normalize(code);
            if (!WubiEncoder::IsValidWubiCode(code)) continue;
            double weight = 10.0;
            if (std::getline(ss, weightStr, '\t')) {
                try { weight = std::stod(weightStr); } catch (...) {}
            }

            BuiltinDictItem item;
            item.text = Utf8ToWide(textStr);
            item.code = code;
            item.weight = weight;
            item.id = "b_wb_" + code + "_" + textStr;

            m_wubiExact[code].push_back(item);
            m_wubiById[item.id] = item;
            m_wubiByText[item.text].push_back(item);
            m_wubiSortedAll.emplace_back(code, std::move(item));
            m_wubiCount++;
        }
    }

    std::sort(m_wubiSortedAll.begin(), m_wubiSortedAll.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    for (auto& pair : m_wubiByText) {
        std::sort(pair.second.begin(), pair.second.end(), [](const BuiltinDictItem& a, const BuiltinDictItem& b) {
            bool aIs4 = (a.code.length() == 4);
            bool bIs4 = (b.code.length() == 4);
            if (aIs4 != bIs4) return aIs4;
            if (a.code.length() != b.code.length()) return a.code.length() > b.code.length();
            return a.weight > b.weight;
        });
    }

    return (m_wubiCount > 0);
}

bool BuiltinDictionary::LoadPinyin(const std::string& filePath) {
    std::ifstream f(filePath);
    if (!f.is_open()) return false;

    m_pinyinExactContinuous.clear();
    m_pinyinExactSpaced.clear();
    m_pinyinSortedContinuous.clear();
    m_pinyinById.clear();
    m_pinyinByText.clear();
    m_pinyinCount = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string textStr, contCode, spacedSyl, weightStr;
        if (std::getline(ss, textStr, '\t') &&
            std::getline(ss, contCode, '\t') &&
            std::getline(ss, spacedSyl, '\t')) {
            double weight = 10.0;
            if (std::getline(ss, weightStr, '\t')) {
                try { weight = std::stod(weightStr); } catch (...) {}
            }

            BuiltinPinyinItem item;
            item.text = Utf8ToWide(textStr);
            item.continuous_code = contCode;
            item.spaced_syllables = spacedSyl;
            item.weight = weight;
            item.id = "b_py_" + contCode + "_" + textStr;

            m_pinyinExactContinuous[contCode].push_back(item);
            m_pinyinExactSpaced[spacedSyl].push_back(item);
            m_pinyinById[item.id] = item;
            m_pinyinByText[item.text].push_back(item);
            m_pinyinSortedContinuous.emplace_back(contCode, std::move(item));
            m_pinyinCount++;
        }
    }

    std::sort(m_pinyinSortedContinuous.begin(), m_pinyinSortedContinuous.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    for (auto& pair : m_pinyinByText) {
        std::sort(pair.second.begin(), pair.second.end(), [](const BuiltinPinyinItem& a, const BuiltinPinyinItem& b) {
            return a.weight > b.weight;
        });
    }

    return (m_pinyinCount > 0);
}

bool BuiltinDictionary::HasItem(const std::string& id) const {
    if (m_wubiById.find(id) != m_wubiById.end()) return true;
    if (m_pinyinById.find(id) != m_pinyinById.end()) return true;
    return false;
}

std::optional<std::wstring> BuiltinDictionary::GetTextById(const std::string& id) const {
    auto itW = m_wubiById.find(id);
    if (itW != m_wubiById.end()) return itW->second.text;
    auto itP = m_pinyinById.find(id);
    if (itP != m_pinyinById.end()) return itP->second.text;
    return std::nullopt;
}

std::optional<Candidate> BuiltinDictionary::GetCandidateById(InputScheme scheme, const std::string& code, const std::string& id) const {
    if (scheme == InputScheme::Wubi86) {
        auto it = m_wubiById.find(id);
        if (it == m_wubiById.end()) return std::nullopt;
        const auto& item = it->second;
        if (item.code.rfind(code, 0) != 0) return std::nullopt;

        Candidate c;
        c.entry_id = item.id;
        c.text = item.text;
        c.scheme = InputScheme::Wubi86;
        c.matched_code = item.code;
        c.quality = (item.code == code) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;
        c.base_weight = item.weight;
        c.source = EntrySource::Builtin;
        return c;
    } else if (scheme == InputScheme::Pinyin) {
        auto it = m_pinyinById.find(id);
        if (it == m_pinyinById.end()) return std::nullopt;
        const auto& item = it->second;
        if (code.find('\'') != std::string::npos) {
            PinyinSegmenter segmenter;
            for (const auto& segmentation : segmenter.Segment(code)) {
                if (segmentation.spaced_syllables == item.spaced_syllables) {
                    Candidate c;
                    c.entry_id = item.id;
                    c.text = item.text;
                    c.scheme = scheme;
                    c.matched_code = item.spaced_syllables;
                    c.quality = MatchQuality::ExactFull;
                    c.base_weight = item.weight;
                    c.source = EntrySource::Builtin;
                    return c;
                }
            }
            return std::nullopt;
        }
        bool matchCont = (item.continuous_code.rfind(code, 0) == 0);
        bool matchSpaced = (item.spaced_syllables.rfind(code, 0) == 0);
        if (!matchCont && !matchSpaced) return std::nullopt;

        Candidate c;
        c.entry_id = item.id;
        c.text = item.text;
        c.scheme = InputScheme::Pinyin;
        c.matched_code = matchCont ? item.continuous_code : item.spaced_syllables;
        c.quality = (c.matched_code == code) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;
        c.base_weight = item.weight;
        c.source = EntrySource::Builtin;
        return c;
    }
    return std::nullopt;
}

std::vector<Candidate> BuiltinDictionary::QueryWubi(const std::string& code) const {
    std::vector<Candidate> result;
    if (code.empty()) return result;

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    // 1. Exact matches (MatchQuality::ExactFull)
    auto itExact = m_wubiExact.find(lowerCode);
    if (itExact != m_wubiExact.end()) {
        for (const auto& item : itExact->second) {
            Candidate c;
            c.entry_id = item.id;
            c.text = item.text;
            c.scheme = InputScheme::Wubi86;
            c.matched_code = item.code;
            c.quality = MatchQuality::ExactFull;
            c.base_weight = item.weight;
            c.source = EntrySource::Builtin;
            result.push_back(std::move(c));
        }
    }

    // 2. Prefix completions (MatchQuality::PrefixCompletion)
    auto itLower = std::lower_bound(m_wubiSortedAll.begin(), m_wubiSortedAll.end(), lowerCode,
        [](const auto& pair, const std::string& val) {
            return pair.first < val;
        });

    static constexpr int MAX_PREFIX_CANDIDATE_POOL = 512;
    int prefixMatches = 0;
    while (itLower != m_wubiSortedAll.end() && prefixMatches < MAX_PREFIX_CANDIDATE_POOL) {
        if (itLower->first.rfind(lowerCode, 0) != 0) {
            break; // No longer starts with lowerCode
        }
        if (itLower->first != lowerCode) {
            const auto& item = itLower->second;
            Candidate c;
            c.entry_id = item.id;
            c.text = item.text;
            c.scheme = InputScheme::Wubi86;
            c.matched_code = item.code;
            c.quality = MatchQuality::PrefixCompletion;
            c.base_weight = item.weight;
            c.source = EntrySource::Builtin;
            result.push_back(std::move(c));
            prefixMatches++;
        }
        ++itLower;
    }

    return result;
}

std::vector<Candidate> BuiltinDictionary::QueryPinyin(const std::string& code, const PinyinSegmenter& segmenter) const {
    std::vector<Candidate> result;
    if (code.empty()) return result;

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto segmentations = segmenter.Segment(lowerCode);

    std::unordered_map<std::wstring, size_t> seen; // text -> index in result

    for (const auto& pseg : segmentations) {
        if (pseg.has_explicit_separator) {
            // Explicit separator: query spaced syllables ONLY!
            auto itSpaced = m_pinyinExactSpaced.find(pseg.spaced_syllables);
            if (itSpaced != m_pinyinExactSpaced.end()) {
                for (const auto& item : itSpaced->second) {
                    if (seen.find(item.text) == seen.end()) {
                        Candidate c;
                        c.entry_id = item.id;
                        c.text = item.text;
                        c.scheme = InputScheme::Pinyin;
                        c.matched_code = pseg.spaced_syllables;
                        c.quality = MatchQuality::ExactFull;
                        c.base_weight = item.weight;
                        c.source = EntrySource::Builtin;
                        seen[item.text] = result.size();
                        result.push_back(std::move(c));
                    }
                }
            }
        } else {
            // 1. Continuous exact match
            auto itCont = m_pinyinExactContinuous.find(lowerCode);
            if (itCont != m_pinyinExactContinuous.end()) {
                for (const auto& item : itCont->second) {
                    // Check if syllable structure matches this segmentation
                    if (item.spaced_syllables == pseg.spaced_syllables || pseg.syllables.size() == 1) {
                        if (seen.find(item.text) == seen.end()) {
                            Candidate c;
                            c.entry_id = item.id;
                            c.text = item.text;
                            c.scheme = InputScheme::Pinyin;
                            c.matched_code = item.continuous_code;
                            c.quality = MatchQuality::ExactFull;
                            c.base_weight = item.weight;
                            c.source = EntrySource::Builtin;
                            seen[item.text] = result.size();
                            result.push_back(std::move(c));
                        }
                    }
                }
            }

            // 2. Also check spaced syllables match for this segmentation
            auto itSpaced = m_pinyinExactSpaced.find(pseg.spaced_syllables);
            if (itSpaced != m_pinyinExactSpaced.end()) {
                for (const auto& item : itSpaced->second) {
                    if (seen.find(item.text) == seen.end()) {
                        Candidate c;
                        c.entry_id = item.id;
                        c.text = item.text;
                        c.scheme = InputScheme::Pinyin;
                        c.matched_code = item.continuous_code;
                        c.quality = MatchQuality::ExactFull;
                        c.base_weight = item.weight;
                        c.source = EntrySource::Builtin;
                        seen[item.text] = result.size();
                        result.push_back(std::move(c));
                    }
                }
            }
        }
    }

    // 3. Prefix completions for continuous code
    auto itLower = std::lower_bound(m_pinyinSortedContinuous.begin(), m_pinyinSortedContinuous.end(), lowerCode,
        [](const auto& pair, const std::string& val) {
            return pair.first < val;
        });

    static constexpr int MAX_PREFIX_CANDIDATE_POOL = 512;
    int prefixMatches = 0;
    while (itLower != m_pinyinSortedContinuous.end() && prefixMatches < MAX_PREFIX_CANDIDATE_POOL) {
        if (itLower->first.rfind(lowerCode, 0) != 0) {
            break;
        }
        if (itLower->first != lowerCode) {
            const auto& item = itLower->second;
            if (seen.find(item.text) == seen.end()) {
                Candidate c;
                c.entry_id = item.id;
                c.text = item.text;
                c.scheme = InputScheme::Pinyin;
                c.matched_code = item.continuous_code;
                c.quality = MatchQuality::PrefixCompletion;
                c.base_weight = item.weight;
                c.source = EntrySource::Builtin;
                seen[item.text] = result.size();
                result.push_back(std::move(c));
                prefixMatches++;
            }
        }
        ++itLower;
    }

    return result;
}

static bool CheckItemSyllablesMatch(const std::string& itemSpaced,
                                    const std::vector<std::string>& reqSyllables,
                                    const std::string& trailingInit,
                                    bool reqComplete) {
    if (reqSyllables.empty() && trailingInit.empty()) return true;

    // Split itemSpaced by ' '
    std::vector<std::string> itemSylls;
    size_t start = 0;
    while (start < itemSpaced.size()) {
        size_t space = itemSpaced.find(' ', start);
        if (space == std::string::npos) {
            itemSylls.push_back(itemSpaced.substr(start));
            break;
        }
        itemSylls.push_back(itemSpaced.substr(start, space - start));
        start = space + 1;
    }

    if (reqComplete) {
        if (itemSylls.size() < reqSyllables.size()) return false;
        for (size_t i = 0; i < reqSyllables.size(); ++i) {
            if (itemSylls[i] != reqSyllables[i]) return false;
        }
        return true;
    } else {
        size_t minRequired = reqSyllables.size() + (!trailingInit.empty() ? 1 : 0);
        if (itemSylls.size() < minRequired) return false;
        for (size_t i = 0; i < reqSyllables.size(); ++i) {
            if (itemSylls[i] != reqSyllables[i]) return false;
        }
        if (!trailingInit.empty()) {
            if (itemSylls[reqSyllables.size()].rfind(trailingInit, 0) != 0) return false;
        }
        return true;
    }
}

bool BuiltinDictionary::CheckPinyinItemMatchesSyllables(
    const std::string& id,
    const std::vector<std::string>& syllables,
    const std::string& trailingInitial,
    bool isComplete) const {
    auto it = m_pinyinById.find(id);
    if (it == m_pinyinById.end()) return false;
    return CheckItemSyllablesMatch(it->second.spaced_syllables, syllables, trailingInitial, isComplete);
}

std::vector<Candidate> BuiltinDictionary::QueryPinyinExplicit(
    const std::vector<std::string>& syllables,
    const std::string& trailingInitial,
    bool isComplete) const {
    std::vector<Candidate> result;
    if (syllables.empty() && trailingInitial.empty()) return result;

    std::unordered_map<std::wstring, size_t> seen;

    // 1. Exact match by spaced_syllables (only possible if isComplete == true)
    if (isComplete && !syllables.empty()) {
        std::string exactSpaced;
        for (size_t i = 0; i < syllables.size(); ++i) {
            if (i > 0) exactSpaced += " ";
            exactSpaced += syllables[i];
        }
        auto itSpaced = m_pinyinExactSpaced.find(exactSpaced);
        if (itSpaced != m_pinyinExactSpaced.end()) {
            for (const auto& item : itSpaced->second) {
                if (seen.find(item.text) == seen.end()) {
                    Candidate c;
                    c.entry_id = item.id;
                    c.text = item.text;
                    c.scheme = InputScheme::Pinyin;
                    c.matched_code = item.spaced_syllables;
                    c.quality = MatchQuality::ExactFull;
                    c.base_weight = item.weight;
                    c.source = EntrySource::Builtin;
                    seen[item.text] = result.size();
                    result.push_back(std::move(c));
                }
            }
        }
    }

    // 2. Prefix completions
    std::string continuousPrefix;
    for (const auto& s : syllables) continuousPrefix += s;
    continuousPrefix += trailingInitial;

    if (!continuousPrefix.empty()) {
        auto itLower = std::lower_bound(m_pinyinSortedContinuous.begin(), m_pinyinSortedContinuous.end(), continuousPrefix,
            [](const auto& pair, const std::string& val) {
                return pair.first < val;
            });

        static constexpr int MAX_PREFIX_CANDIDATE_POOL = 512;
        int prefixMatches = 0;
        while (itLower != m_pinyinSortedContinuous.end() && prefixMatches < MAX_PREFIX_CANDIDATE_POOL) {
            if (itLower->first.rfind(continuousPrefix, 0) != 0) {
                break;
            }
            const auto& item = itLower->second;
            if (seen.find(item.text) == seen.end()) {
                if (CheckItemSyllablesMatch(item.spaced_syllables, syllables, trailingInitial, isComplete)) {
                    Candidate c;
                    c.entry_id = item.id;
                    c.text = item.text;
                    c.scheme = InputScheme::Pinyin;
                    c.matched_code = item.continuous_code;
                    c.quality = (isComplete && item.continuous_code == continuousPrefix)
                        ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;
                    c.base_weight = item.weight;
                    c.source = EntrySource::Builtin;
                    seen[item.text] = result.size();
                    result.push_back(std::move(c));
                    prefixMatches++;
                }
            }
            ++itLower;
        }
    }

    return result;
}

std::vector<std::string> BuiltinDictionary::ReverseLookupWubi(const std::wstring& text) const {
    std::vector<std::string> codes;
    auto it = m_wubiByText.find(text);
    if (it == m_wubiByText.end() || it->second.empty()) {
        return codes;
    }
    std::unordered_set<std::string> seen;
    for (const auto& item : it->second) {
        if (!item.code.empty() && seen.insert(item.code).second) {
            codes.push_back(item.code);
        }
    }
    return codes;
}

std::vector<std::string> BuiltinDictionary::ReverseLookupPinyin(const std::wstring& text) const {
    std::vector<std::string> codes;
    auto it = m_pinyinByText.find(text);
    if (it == m_pinyinByText.end() || it->second.empty()) {
        return codes;
    }
    std::unordered_set<std::string> seen;
    for (const auto& item : it->second) {
        if (!item.continuous_code.empty() && seen.insert(item.continuous_code).second) {
            codes.push_back(item.continuous_code);
        }
    }
    return codes;
}

std::vector<std::string> BuiltinDictionary::ReverseLookupShuangpin(const std::wstring& text, const ShuangpinDecoder& decoder) const {
    std::vector<std::string> codes;
    auto it = m_pinyinByText.find(text);
    if (it == m_pinyinByText.end() || it->second.empty()) {
        return codes;
    }
    std::unordered_set<std::string> seen;
    for (const auto& item : it->second) {
        if (item.spaced_syllables.empty()) continue;
        std::stringstream ss(item.spaced_syllables);
        std::string syl;
        std::vector<std::string> syls;
        while (ss >> syl) {
            syls.push_back(syl);
        }
        std::string sp = decoder.EncodePinyin(syls);
        if (sp.empty()) continue;
        auto decRes = decoder.Decode(sp);
        if (!decRes.is_valid || !decRes.is_complete) continue;
        bool matchesBranch = false;
        for (const auto& branch : decRes.branches) {
            if (branch.syllables == syls) {
                matchesBranch = true;
                break;
            }
        }
        if (!matchesBranch && decRes.syllables != syls) continue;
        if (seen.insert(sp).second) {
            codes.push_back(sp);
        }
    }
    return codes;
}

std::vector<Entry> BuiltinDictionary::SearchEntries(const std::wstring& keywordText, const std::string& keywordCode, size_t maxResults) const {
    std::vector<Entry> results;
    if (keywordText.empty() && keywordCode.empty()) {
        return results;
    }

    std::vector<std::wstring> matchedTexts;
    std::unordered_set<std::wstring> seenTexts;

    auto addMatch = [&](const std::wstring& text) {
        if (!text.empty() && seenTexts.insert(text).second) {
            matchedTexts.push_back(text);
        }
    };

    // 1. Text lookup: exact match first
    if (!keywordText.empty()) {
        if (m_pinyinByText.find(keywordText) != m_pinyinByText.end() ||
            m_wubiByText.find(keywordText) != m_wubiByText.end()) {
            addMatch(keywordText);
        }

        // Substring matches in pinyin dictionary
        for (const auto& pair : m_pinyinByText) {
            if (matchedTexts.size() >= maxResults) break;
            if (pair.first.find(keywordText) != std::wstring::npos) {
                addMatch(pair.first);
            }
        }

        // Substring matches in wubi dictionary
        for (const auto& pair : m_wubiByText) {
            if (matchedTexts.size() >= maxResults) break;
            if (pair.first.find(keywordText) != std::wstring::npos) {
                addMatch(pair.first);
            }
        }
    }

    // 2. Code lookup (if keywordCode provided)
    if (!keywordCode.empty() && matchedTexts.size() < maxResults) {
        // Continuous pinyin exact match
        auto itP = m_pinyinExactContinuous.find(keywordCode);
        if (itP != m_pinyinExactContinuous.end()) {
            for (const auto& item : itP->second) {
                if (matchedTexts.size() >= maxResults) break;
                addMatch(item.text);
            }
        }

        // Wubi exact match
        auto itW = m_wubiExact.find(keywordCode);
        if (itW != m_wubiExact.end()) {
            for (const auto& item : itW->second) {
                if (matchedTexts.size() >= maxResults) break;
                addMatch(item.text);
            }
        }
    }

    // 3. Assemble Entry objects with their respective codes
    results.reserve(matchedTexts.size());
    for (const auto& text : matchedTexts) {
        Entry e;
        e.id = "builtin:" + WideToUtf8(text);
        e.text = text;
        e.source = EntrySource::Builtin;

        auto wubiCodes = ReverseLookupWubi(text);
        for (const auto& c : wubiCodes) {
            e.codes.push_back({InputScheme::Wubi86, c});
        }

        auto pyCodes = ReverseLookupPinyin(text);
        for (const auto& c : pyCodes) {
            e.codes.push_back({InputScheme::Pinyin, c});
        }

        results.push_back(std::move(e));
    }

    return results;
}

} // namespace ime

