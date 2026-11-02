#include "builtin_dict.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

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
    m_wubiCount = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string textStr, code, weightStr;
        if (std::getline(ss, textStr, '\t') && std::getline(ss, code, '\t')) {
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
            m_wubiSortedAll.emplace_back(code, std::move(item));
            m_wubiCount++;
        }
    }

    std::sort(m_wubiSortedAll.begin(), m_wubiSortedAll.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    return (m_wubiCount > 0);
}

bool BuiltinDictionary::LoadPinyin(const std::string& filePath) {
    std::ifstream f(filePath);
    if (!f.is_open()) return false;

    m_pinyinExactContinuous.clear();
    m_pinyinExactSpaced.clear();
    m_pinyinSortedContinuous.clear();
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
            m_pinyinSortedContinuous.emplace_back(contCode, std::move(item));
            m_pinyinCount++;
        }
    }

    std::sort(m_pinyinSortedContinuous.begin(), m_pinyinSortedContinuous.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    return (m_pinyinCount > 0);
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

} // namespace ime

