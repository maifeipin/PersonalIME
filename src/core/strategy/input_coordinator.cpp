#include "input_coordinator.h"
#include <algorithm>
#include <unordered_set>
#include <stdexcept>

namespace ime {

InputCoordinator::InputCoordinator(EntryRepository& repo,
                                   const BuiltinDictionary& builtinDict,
                                   std::shared_ptr<IClock> clock)
    : m_repo(repo)
    , m_builtinDict(builtinDict)
    , m_clock(clock ? clock : std::make_shared<SystemClock>())
    , m_ranker(m_clock)
    , m_decoderNatural(ShuangpinLayout::Natural)
    , m_decoderMS(ShuangpinLayout::Microsoft)
    , m_decoderXiaohe(ShuangpinLayout::Xiaohe) {
    m_repo.SetBuiltinDictionary(&m_builtinDict);
    m_currentPolicy = std::make_shared<InputPolicy>();
}

const ShuangpinDecoder& InputCoordinator::GetShuangpinDecoder(ShuangpinLayout layout) const {
    switch (layout) {
        case ShuangpinLayout::Natural: return m_decoderNatural;
        case ShuangpinLayout::Microsoft: return m_decoderMS;
        case ShuangpinLayout::Xiaohe: return m_decoderXiaohe;
        default: return m_decoderNatural;
    }
}

std::shared_ptr<const InputPolicy> InputCoordinator::GetPolicySnapshot() const {
    std::lock_guard<std::mutex> lock(m_policyMutex);
    return m_currentPolicy;
}

void InputCoordinator::SetPolicy(const InputPolicy& policy) {
    std::lock_guard<std::mutex> lock(m_policyMutex);
    auto next = std::make_shared<InputPolicy>(policy);
    uint64_t nextRev = m_currentPolicy ? m_currentPolicy->revision + 1 : 1;
    next->revision = (std::max)(policy.revision, nextRev);
    m_currentPolicy = next;
}

void InputCoordinator::UpdatePolicy(const InputPolicy& policy) {
    SetPolicy(policy);
}

std::string InputCoordinator::BuildScopedCode(InputScheme scheme,
                                              const std::string& layoutId,
                                              const std::string& code) {
    if (scheme == InputScheme::Shuangpin && !layoutId.empty()) {
        ShuangpinLayout lay = ShuangpinLayoutFromString(layoutId);
        std::string canonical = ShuangpinLayoutToString(lay);
        return canonical + ":" + code;
    }
    return code;
}

bool InputCoordinator::SetPinPreference(InputScheme scheme,
                                        const std::string& code,
                                        const std::string& entryId,
                                        bool isPinned,
                                        int orderRank,
                                        const std::string& layoutId,
                                        std::optional<uint64_t> expectedRevision,
                                        uint64_t* outRevision,
                                        bool* outConflict) {
    std::string scoped = BuildScopedCode(scheme, layoutId, code);
    return m_repo.SetPinPreference(scheme, scoped, entryId, isPinned, orderRank, expectedRevision, outRevision, outConflict);
}

bool InputCoordinator::UnpinPreference(InputScheme scheme,
                                       const std::string& code,
                                       const std::string& entryId,
                                       const std::string& layoutId,
                                       std::optional<uint64_t> expectedRevision,
                                       uint64_t* outRevision,
                                       bool* outConflict) {
    std::string scoped = BuildScopedCode(scheme, layoutId, code);
    return m_repo.UnpinPreference(scheme, scoped, entryId, expectedRevision, outRevision, outConflict);
}

std::vector<InputCoordinator::MethodResolution>
InputCoordinator::ResolveInputMethods(const std::string& rawCode,
                                     const InputPolicy& policy) const {
    std::vector<MethodResolution> resList;
    if (rawCode.empty()) return resList;

    // Custom phrase prefix check (e.g. ';dz', ';dh', ';bq')
    if (rawCode.front() == ';') {
        MethodResolution customRes;
        customRes.scheme = InputScheme::Custom;
        customRes.raw_code = rawCode;
        customRes.resolved_code = rawCode;
        customRes.layout_id = "";
        customRes.method_priority = 0;
        customRes.is_valid = IsValidCode(InputScheme::Custom, rawCode);
        customRes.is_complete = true;
        if (customRes.is_valid) {
            resList.push_back(customRes);
        }
    }

    auto activeMethods = policy.GetActiveInputMethods();
    for (size_t i = 0; i < activeMethods.size(); ++i) {
        auto method = activeMethods[i];
        int methodPriority = static_cast<int>(i);

        if (method == InputScheme::Wubi86) {
            MethodResolution res;
            res.scheme = InputScheme::Wubi86;
            res.raw_code = rawCode;
            res.resolved_code = rawCode;
            res.layout_id = "";
            res.method_priority = methodPriority;
            res.is_valid = IsValidCode(InputScheme::Wubi86, rawCode);
            res.is_complete = true;
            if (res.is_valid) resList.push_back(res);
        } else if (method == InputScheme::Pinyin) {
            if (!IsValidCode(InputScheme::Pinyin, rawCode)) {
                continue;
            }

            std::string clean = rawCode;
            std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

            bool hasSep = (clean.find('\'') != std::string::npos);
            if (hasSep) {
                std::vector<std::string> parts;
                std::string cur;
                for (char c : clean) {
                    if (c == '\'') {
                        if (!cur.empty()) {
                            parts.push_back(cur);
                            cur.clear();
                        }
                    } else {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty()) parts.push_back(cur);

                if (!parts.empty()) {
                    MethodResolution res;
                    res.scheme = InputScheme::Pinyin;
                    res.raw_code = rawCode;
                    res.layout_id = "";
                    res.method_priority = methodPriority;
                    res.is_valid = true;

                    if (m_pinyinSegmenter.IsValidSyllable(parts.back())) {
                        res.syllables = parts;
                        res.trailing_initial = "";
                        res.is_complete = true;
                    } else {
                        res.syllables = std::vector<std::string>(parts.begin(), parts.end() - 1);
                        res.trailing_initial = parts.back();
                        res.is_complete = false;
                    }

                    for (size_t p = 0; p < res.syllables.size(); ++p) {
                        if (p > 0) res.spaced_code += " ";
                        res.spaced_code += res.syllables[p];
                        res.resolved_code += res.syllables[p];
                    }
                    if (!res.trailing_initial.empty()) {
                        if (!res.spaced_code.empty()) res.spaced_code += " ";
                        res.spaced_code += res.trailing_initial;
                        res.resolved_code += res.trailing_initial;
                    }

                    resList.push_back(res);
                }
            } else {
                // Continuous input without apostrophe: e.g. "z", "zh", "zhong", "zhongw", "zhongwen", "xian"
                auto segs = m_pinyinSegmenter.Segment(clean);
                bool hasCompleteSeg = false;
                for (const auto& seg : segs) {
                    bool allValid = true;
                    for (const auto& s : seg.syllables) {
                        if (!m_pinyinSegmenter.IsValidSyllable(s)) {
                            allValid = false;
                            break;
                        }
                    }
                    if (allValid && !seg.syllables.empty()) {
                        hasCompleteSeg = true;
                        MethodResolution res;
                        res.scheme = InputScheme::Pinyin;
                        res.raw_code = rawCode;
                        res.resolved_code = seg.raw_continuous;
                        res.spaced_code = seg.spaced_syllables;
                        res.syllables = seg.syllables;
                        res.trailing_initial = "";
                        res.layout_id = "";
                        res.method_priority = methodPriority;
                        res.is_valid = true;
                        res.is_complete = true;
                        resList.push_back(res);
                    }
                }

                if (!hasCompleteSeg) {
                    // Incomplete syllable / trailing prefix: find longest prefix that segments into valid syllables
                    MethodResolution res;
                    res.scheme = InputScheme::Pinyin;
                    res.raw_code = rawCode;
                    res.layout_id = "";
                    res.method_priority = methodPriority;
                    res.is_valid = true;
                    res.is_complete = false;

                    bool foundPrefix = false;
                    for (int pos = static_cast<int>(clean.length()) - 1; pos >= 1; --pos) {
                        std::string sub = clean.substr(0, pos);
                        auto subSegs = m_pinyinSegmenter.Segment(sub);
                        for (const auto& sseg : subSegs) {
                            bool allValid = true;
                            for (const auto& s : sseg.syllables) {
                                if (!m_pinyinSegmenter.IsValidSyllable(s)) {
                                    allValid = false;
                                    break;
                                }
                            }
                            if (allValid && !sseg.syllables.empty()) {
                                res.syllables = sseg.syllables;
                                res.trailing_initial = clean.substr(pos);
                                res.resolved_code = clean;
                                res.spaced_code = sseg.spaced_syllables + " " + res.trailing_initial;
                                foundPrefix = true;
                                break;
                            }
                        }
                        if (foundPrefix) break;
                    }

                    if (!foundPrefix) {
                        // Entire string is a single trailing initial / prefix (e.g. "z", "zh")
                        res.syllables = {};
                        res.trailing_initial = clean;
                        res.resolved_code = clean;
                        res.spaced_code = clean;
                    }

                    resList.push_back(res);
                }
            }
        } else if (method == InputScheme::Shuangpin) {
            const auto& decoder = GetShuangpinDecoder(policy.shuangpin_layout);
            auto sp = decoder.Decode(rawCode);
            if (sp.is_valid && (!sp.syllables.empty() || !sp.trailing_incomplete.empty())) {
                for (const auto& branch : sp.branches) {
                    MethodResolution res;
                    res.scheme = InputScheme::Shuangpin;
                    res.raw_code = rawCode;
                    res.resolved_code = branch.continuous;
                    res.spaced_code = branch.spaced;
                    res.syllables = branch.syllables;
                    res.trailing_initial = branch.trailing_initial;
                    res.layout_id = ShuangpinLayoutToString(policy.shuangpin_layout);
                    res.method_priority = methodPriority;
                    res.is_valid = true;
                    res.is_complete = sp.is_complete;
                    res.is_truncated = sp.is_truncated;
                    resList.push_back(res);
                }
            }
        }
    }

    return resList;
}

static std::vector<std::string> ParseCustomPinyinSyllables(const std::string& code, const PinyinSegmenter& segmenter) {
    std::string clean = code;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

    // Check for explicit separators ' or space
    if (clean.find('\'') != std::string::npos || clean.find(' ') != std::string::npos) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : clean) {
            if (c == '\'' || c == ' ') {
                if (!cur.empty()) {
                    parts.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) parts.push_back(cur);
        return parts;
    }

    // Unseparated continuous pinyin: canonical segmentation from segmenter
    auto segs = segmenter.Segment(clean);
    if (!segs.empty() && !segs.front().syllables.empty()) {
        return segs.front().syllables;
    }
    return {clean};
}

static std::optional<MatchQuality> MatchCustomPinyinCandidate(
    const std::vector<std::string>& candSylls,
    const std::vector<std::string>& reqSylls,
    const std::string& trailingInit,
    bool reqComplete,
    bool allowLastSyllablePrefix = false) {

    if (reqSylls.empty() && trailingInit.empty()) {
        return std::nullopt;
    }

    if (reqComplete) {
        if (candSylls.size() < reqSylls.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < reqSylls.size(); ++i) {
            if (candSylls[i] != reqSylls[i]) {
                // Continuous full pinyin can still extend a valid final syllable:
                // a -> an, xi -> xian. Double-pinyin pairs and explicit separators
                // retain their established syllable boundaries.
                if (allowLastSyllablePrefix && i + 1 == reqSylls.size() &&
                    candSylls[i].rfind(reqSylls[i], 0) == 0) {
                    return MatchQuality::PrefixCompletion;
                }
                return std::nullopt;
            }
        }
        if (candSylls.size() == reqSylls.size()) {
            return MatchQuality::ExactFull;
        } else {
            return MatchQuality::PrefixCompletion;
        }
    } else {
        size_t minRequired = reqSylls.size() + (!trailingInit.empty() ? 1 : 0);
        if (candSylls.size() < minRequired) {
            return std::nullopt;
        }
        for (size_t i = 0; i < reqSylls.size(); ++i) {
            if (candSylls[i] != reqSylls[i]) {
                return std::nullopt;
            }
        }
        if (!trailingInit.empty()) {
            if (candSylls[reqSylls.size()].rfind(trailingInit, 0) != 0) {
                return std::nullopt;
            }
        }
        return MatchQuality::PrefixCompletion;
    }
}

std::vector<Candidate>
InputCoordinator::QuerySingleDictionary(const std::string& dictId,
                                       const MethodResolution& res,
                                       bool exactOnly) const {
    std::vector<Candidate> candidates;

    if (dictId == DICT_ID_CUSTOM) {
        if (res.scheme == InputScheme::Wubi86) {
            auto userCands = m_repo.QueryUserCandidates(InputScheme::Wubi86, res.resolved_code);
            std::unordered_map<std::string, size_t> candMap;
            for (auto& c : userCands) {
                if (exactOnly && c.quality != MatchQuality::ExactFull) continue;
                auto it = candMap.find(c.entry_id);
                if (it != candMap.end()) {
                    if (static_cast<int>(c.quality) < static_cast<int>(candidates[it->second].quality)) {
                        candidates[it->second].quality = c.quality;
                        candidates[it->second].matched_code = c.matched_code;
                    }
                    continue;
                }
                c.dict_id = DICT_ID_CUSTOM;
                c.layout_id = res.layout_id;
                c.raw_code = res.raw_code;
                c.resolved_code = res.resolved_code;
                c.scheme = res.scheme;
                c.method_priority = res.method_priority;
                candMap[c.entry_id] = candidates.size();
                candidates.push_back(std::move(c));
            }
        } else if (res.scheme == InputScheme::Custom) {
            auto userCands = m_repo.QueryUserCandidates(InputScheme::Custom, res.raw_code);
            std::unordered_map<std::string, size_t> candMap;
            for (auto& c : userCands) {
                if (exactOnly && c.quality != MatchQuality::ExactFull) continue;
                auto it = candMap.find(c.entry_id);
                if (it != candMap.end()) {
                    if (static_cast<int>(c.quality) < static_cast<int>(candidates[it->second].quality)) {
                        candidates[it->second].quality = c.quality;
                        candidates[it->second].matched_code = c.matched_code;
                    }
                    continue;
                }
                c.dict_id = DICT_ID_CUSTOM;
                c.layout_id = res.layout_id;
                c.raw_code = res.raw_code;
                c.resolved_code = res.resolved_code;
                c.scheme = res.scheme;
                c.method_priority = res.method_priority;
                candMap[c.entry_id] = candidates.size();
                candidates.push_back(std::move(c));
            }
        } else if (res.scheme == InputScheme::Pinyin || res.scheme == InputScheme::Shuangpin) {
            std::unordered_map<std::string, size_t> candMap;

            // 1. If Shuangpin, query custom entries registered directly under Shuangpin scheme with raw_code
            if (res.scheme == InputScheme::Shuangpin) {
                auto spCands = m_repo.QueryUserCandidates(InputScheme::Shuangpin, res.raw_code);
                for (auto& c : spCands) {
                    if (exactOnly && c.quality != MatchQuality::ExactFull) continue;
                    auto it = candMap.find(c.entry_id);
                    if (it != candMap.end()) {
                        if (static_cast<int>(c.quality) < static_cast<int>(candidates[it->second].quality)) {
                            candidates[it->second].quality = c.quality;
                            candidates[it->second].matched_code = c.matched_code;
                        }
                        continue;
                    }
                    c.dict_id = DICT_ID_CUSTOM;
                    c.layout_id = res.layout_id;
                    c.raw_code = res.raw_code;
                    c.resolved_code = res.resolved_code;
                    c.scheme = res.scheme;
                    c.method_priority = res.method_priority;
                    candMap[c.entry_id] = candidates.size();
                    candidates.push_back(std::move(c));
                }
            }

            // 2. Query custom entries registered under Pinyin
            // Candidate search prefixes in SQLite
            std::vector<std::string> searchCodes;
            if (!res.syllables.empty()) {
                searchCodes.push_back(res.syllables[0]);
            } else if (!res.trailing_initial.empty()) {
                searchCodes.push_back(res.trailing_initial);
            }
            if (!res.resolved_code.empty() && std::find(searchCodes.begin(), searchCodes.end(), res.resolved_code) == searchCodes.end()) {
                searchCodes.push_back(res.resolved_code);
            }
            if (!res.raw_code.empty() && std::find(searchCodes.begin(), searchCodes.end(), res.raw_code) == searchCodes.end()) {
                searchCodes.push_back(res.raw_code);
            }

            std::vector<Candidate> pyCands;
            for (const auto& sc : searchCodes) {
                auto pool = m_repo.QueryUserCandidates(InputScheme::Pinyin, sc);
                for (auto& pc : pool) {
                    pyCands.push_back(std::move(pc));
                }
            }

            for (auto& c : pyCands) {
                std::string cMatched = c.matched_code;
                std::transform(cMatched.begin(), cMatched.end(), cMatched.begin(), [](unsigned char ch) { return std::tolower(ch); });
                bool isExactCode = (res.scheme == InputScheme::Pinyin && cMatched == res.raw_code);

                auto candSylls = ParseCustomPinyinSyllables(c.matched_code, m_pinyinSegmenter);
                bool allowLastSyllablePrefix = res.scheme == InputScheme::Pinyin &&
                    res.raw_code.find('\'') == std::string::npos;
                auto qualOpt = MatchCustomPinyinCandidate(candSylls, res.syllables,
                    res.trailing_initial, res.is_complete, allowLastSyllablePrefix);
                if (!qualOpt.has_value() && !isExactCode) {
                    continue; // Does not match syllable boundaries / initial
                }

                c.quality = isExactCode ? MatchQuality::ExactFull : qualOpt.value();
                if (exactOnly && c.quality != MatchQuality::ExactFull) continue;

                auto it = candMap.find(c.entry_id);
                if (it != candMap.end()) {
                    Candidate& existing = candidates[it->second];
                    if (static_cast<int>(c.quality) < static_cast<int>(existing.quality)) {
                        existing.quality = c.quality;
                        existing.matched_code = c.matched_code;
                    }
                    continue;
                }

                c.dict_id = DICT_ID_CUSTOM;
                c.layout_id = res.layout_id;
                c.raw_code = res.raw_code;
                c.resolved_code = res.resolved_code;
                c.scheme = res.scheme;
                c.method_priority = res.method_priority;
                candMap[c.entry_id] = candidates.size();
                candidates.push_back(std::move(c));
            }
        }
    } else if (dictId == DICT_ID_BUILTIN_WUBI) {
        if (res.scheme == InputScheme::Wubi86) {
            auto wubiCands = m_builtinDict.QueryWubi(res.resolved_code);
            std::unordered_set<std::string> seenIds;
            for (auto& c : wubiCands) {
                if (exactOnly && c.quality != MatchQuality::ExactFull) continue;
                c.dict_id = DICT_ID_BUILTIN_WUBI;
                c.layout_id = "";
                c.raw_code = res.raw_code;
                c.resolved_code = res.resolved_code;
                c.scheme = InputScheme::Wubi86;
                c.method_priority = res.method_priority;
                seenIds.insert(c.entry_id);
                candidates.push_back(std::move(c));
            }

            // Recall pinned builtin items outside bounded pool
            std::string scopedRecallKey = BuildScopedCode(res.scheme, res.layout_id, res.raw_code);
            auto pinnedPrefs = m_repo.GetPinnedPreferences(InputScheme::Wubi86, scopedRecallKey);
            for (const auto& pref : pinnedPrefs) {
                if (seenIds.count(pref.entry_id) == 0 && pref.entry_id.rfind("b_", 0) == 0) {
                    auto cand = m_builtinDict.GetCandidateById(InputScheme::Wubi86, res.resolved_code, pref.entry_id);
                    if (cand) {
                        if (exactOnly && cand->quality != MatchQuality::ExactFull) continue;
                        cand->dict_id = DICT_ID_BUILTIN_WUBI;
                        cand->layout_id = "";
                        cand->raw_code = res.raw_code;
                        cand->resolved_code = res.resolved_code;
                        cand->scheme = InputScheme::Wubi86;
                        cand->method_priority = res.method_priority;
                        seenIds.insert(pref.entry_id);
                        candidates.push_back(std::move(*cand));
                    }
                }
            }

            // Recall active stat builtin items outside bounded pool
            auto activeStats = m_repo.GetActiveStats(InputScheme::Wubi86, scopedRecallKey);
            for (const auto& stat : activeStats) {
                if (seenIds.count(stat.entry_id) == 0 && stat.entry_id.rfind("b_", 0) == 0) {
                    auto cand = m_builtinDict.GetCandidateById(InputScheme::Wubi86, res.resolved_code, stat.entry_id);
                    if (cand) {
                        if (exactOnly && cand->quality != MatchQuality::ExactFull) continue;
                        cand->dict_id = DICT_ID_BUILTIN_WUBI;
                        cand->layout_id = "";
                        cand->raw_code = res.raw_code;
                        cand->resolved_code = res.resolved_code;
                        cand->scheme = InputScheme::Wubi86;
                        cand->method_priority = res.method_priority;
                        seenIds.insert(stat.entry_id);
                        candidates.push_back(std::move(*cand));
                    }
                }
            }
        }
    } else if (dictId == DICT_ID_BUILTIN_PINYIN) {
        if (res.scheme == InputScheme::Pinyin || res.scheme == InputScheme::Shuangpin) {
            // Preserve explicit full-pinyin separators for both normal lookup and
            // pool-external recall; the continuous form loses this constraint.
            std::string dictionaryQueryCode = res.scheme == InputScheme::Pinyin
                ? res.raw_code : res.resolved_code;
            std::transform(dictionaryQueryCode.begin(), dictionaryQueryCode.end(),
                dictionaryQueryCode.begin(), [](unsigned char c) { return std::tolower(c); });
            std::vector<Candidate> pinyinCands;
            if (res.scheme == InputScheme::Shuangpin) {
                pinyinCands = m_builtinDict.QueryPinyinExplicit(res.syllables, res.trailing_initial, res.is_complete);
            } else {
                pinyinCands = m_builtinDict.QueryPinyin(dictionaryQueryCode, m_pinyinSegmenter);
            }
            std::unordered_set<std::string> seenIds;
            for (auto& c : pinyinCands) {
                if (exactOnly && c.quality != MatchQuality::ExactFull) continue;
                c.dict_id = DICT_ID_BUILTIN_PINYIN;
                c.layout_id = res.layout_id;
                c.raw_code = res.raw_code;
                c.resolved_code = res.resolved_code;
                c.scheme = res.scheme;
                c.method_priority = res.method_priority;
                seenIds.insert(c.entry_id);
                candidates.push_back(std::move(c));
            }

            // Recall pinned builtin items outside bounded pool (P1-1 & P1-2)
            std::string scopedRecallKey = BuildScopedCode(res.scheme, res.layout_id, res.raw_code);
            InputScheme recallScheme = res.scheme;
            auto pinnedPrefs = m_repo.GetPinnedPreferences(recallScheme, scopedRecallKey);
            for (const auto& pref : pinnedPrefs) {
                if (seenIds.count(pref.entry_id) == 0 && pref.entry_id.rfind("b_", 0) == 0) {
                    auto cand = m_builtinDict.GetCandidateById(InputScheme::Pinyin, dictionaryQueryCode, pref.entry_id);
                    if (cand) {
                        if (!res.is_complete && cand->quality == MatchQuality::ExactFull) {
                            cand->quality = MatchQuality::PrefixCompletion;
                        }
                        if (exactOnly && cand->quality != MatchQuality::ExactFull) continue;
                        if (res.scheme == InputScheme::Shuangpin && !res.syllables.empty()) {
                            if (!m_builtinDict.CheckPinyinItemMatchesSyllables(cand->entry_id, res.syllables, res.trailing_initial, res.is_complete)) {
                                continue;
                            }
                        }
                        cand->dict_id = DICT_ID_BUILTIN_PINYIN;
                        cand->layout_id = res.layout_id;
                        cand->raw_code = res.raw_code;
                        cand->resolved_code = res.resolved_code;
                        cand->scheme = res.scheme;
                        cand->method_priority = res.method_priority;
                        seenIds.insert(pref.entry_id);
                        candidates.push_back(std::move(*cand));
                    }
                }
            }

            // Recall active stat builtin items outside bounded pool (P1-1 & P1-2)
            auto activeStats = m_repo.GetActiveStats(recallScheme, scopedRecallKey);
            for (const auto& stat : activeStats) {
                if (seenIds.count(stat.entry_id) == 0 && stat.entry_id.rfind("b_", 0) == 0) {
                    auto cand = m_builtinDict.GetCandidateById(InputScheme::Pinyin, dictionaryQueryCode, stat.entry_id);
                    if (cand) {
                        if (!res.is_complete && cand->quality == MatchQuality::ExactFull) {
                            cand->quality = MatchQuality::PrefixCompletion;
                        }
                        if (exactOnly && cand->quality != MatchQuality::ExactFull) continue;
                        if (res.scheme == InputScheme::Shuangpin && !res.syllables.empty()) {
                            if (!m_builtinDict.CheckPinyinItemMatchesSyllables(cand->entry_id, res.syllables, res.trailing_initial, res.is_complete)) {
                                continue;
                            }
                        }
                        cand->dict_id = DICT_ID_BUILTIN_PINYIN;
                        cand->layout_id = res.layout_id;
                        cand->raw_code = res.raw_code;
                        cand->resolved_code = res.resolved_code;
                        cand->scheme = res.scheme;
                        cand->method_priority = res.method_priority;
                        seenIds.insert(stat.entry_id);
                        candidates.push_back(std::move(*cand));
                    }
                }
            }
        }
    }

    return candidates;
}

std::vector<Candidate> InputCoordinator::QueryScopedCandidates(InputScheme scheme,
                                                              const std::string& layoutId,
                                                              const std::string& code,
                                                              const std::string& keyword) const {
    std::vector<Candidate> result;
    if (code.empty()) return result;

    InputPolicy scopePolicy;
    scopePolicy.primary_method = scheme;
    scopePolicy.mixed_input_enabled = false;
    if (scheme == InputScheme::Shuangpin && !layoutId.empty()) {
        scopePolicy.shuangpin_layout = ShuangpinLayoutFromString(layoutId);
    }

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string lowerKw = keyword;
    std::transform(lowerKw.begin(), lowerKw.end(), lowerKw.begin(), [](unsigned char c) { return std::tolower(c); });

    auto resolutions = ResolveInputMethods(lowerCode, scopePolicy);
    if (resolutions.empty()) {
        return result;
    }

    std::unordered_set<std::string> seenIds;
    auto addCandidate = [&](Candidate&& c) {
        if (!lowerKw.empty()) {
            std::string textUtf8 = WideToUtf8(c.text);
            std::string textLower = textUtf8;
            std::transform(textLower.begin(), textLower.end(), textLower.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::string matchLower = c.matched_code;
            std::transform(matchLower.begin(), matchLower.end(), matchLower.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::string rawLower = c.raw_code;
            std::transform(rawLower.begin(), rawLower.end(), rawLower.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (textLower.find(lowerKw) == std::string::npos &&
                matchLower.find(lowerKw) == std::string::npos &&
                rawLower.find(lowerKw) == std::string::npos) {
                return;
            }
        }
        if (seenIds.insert(c.entry_id).second) {
            result.push_back(std::move(c));
        }
    };

    for (const auto& res : resolutions) {
        // 1. Query custom dictionary
        auto customCands = QuerySingleDictionary(DICT_ID_CUSTOM, res, false);
        for (auto& c : customCands) {
            addCandidate(std::move(c));
        }

        // 2. Query scheme-specific builtin dictionary
        if (res.scheme == InputScheme::Wubi86) {
            auto builtinCands = QuerySingleDictionary(DICT_ID_BUILTIN_WUBI, res, false);
            for (auto& c : builtinCands) {
                addCandidate(std::move(c));
            }
        } else if (res.scheme == InputScheme::Pinyin || res.scheme == InputScheme::Shuangpin) {
            auto builtinCands = QuerySingleDictionary(DICT_ID_BUILTIN_PINYIN, res, false);
            for (auto& c : builtinCands) {
                addCandidate(std::move(c));
            }
        }
    }

    return result;
}

bool InputCoordinator::IsEntryValidForScope(InputScheme scheme,
                                            const std::string& layoutId,
                                            const std::string& code,
                                            const std::string& entryId) const {
    if (code.empty() || entryId.empty()) return false;

    InputPolicy scopePolicy;
    scopePolicy.primary_method = scheme;
    scopePolicy.mixed_input_enabled = false;
    if (scheme == InputScheme::Shuangpin && !layoutId.empty()) {
        scopePolicy.shuangpin_layout = ShuangpinLayoutFromString(layoutId);
    }

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto resolutions = ResolveInputMethods(lowerCode, scopePolicy);
    if (resolutions.empty()) {
        return false;
    }

    // 1. Check builtin dictionary candidates (identified by stable b_ prefix)
    if (entryId.rfind("b_", 0) == 0) {
        for (const auto& res : resolutions) {
            if (res.scheme == InputScheme::Wubi86) {
                auto bCand = m_builtinDict.GetCandidateById(InputScheme::Wubi86, res.resolved_code, entryId);
                if (bCand.has_value()) return true;
            } else if (res.scheme == InputScheme::Pinyin || res.scheme == InputScheme::Shuangpin) {
                std::string dictCode = (res.scheme == InputScheme::Pinyin) ? res.raw_code : res.resolved_code;
                auto bCand = m_builtinDict.GetCandidateById(InputScheme::Pinyin, dictCode, entryId);
                if (bCand.has_value()) {
                    if (res.scheme == InputScheme::Shuangpin && !res.syllables.empty()) {
                        if (!m_builtinDict.CheckPinyinItemMatchesSyllables(entryId, res.syllables, res.trailing_initial, res.is_complete)) {
                            continue;
                        }
                    }
                    return true;
                }
            }
        }
        return false;
    }

    // 2. Check user repository entries
    auto userEntry = m_repo.GetEntry(entryId);
    if (userEntry) {
        for (const auto& ec : userEntry->codes) {
            for (const auto& res : resolutions) {
                if (res.scheme == InputScheme::Wubi86 && ec.first == InputScheme::Wubi86) {
                    std::string c = ec.second;
                    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    if (c == res.resolved_code || c.rfind(res.resolved_code, 0) == 0) {
                        return true;
                    }
                } else if (res.scheme == InputScheme::Custom && ec.first == InputScheme::Custom) {
                    std::string c = ec.second;
                    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    if (c == res.raw_code) {
                        return true;
                    }
                } else if (res.scheme == InputScheme::Shuangpin && ec.first == InputScheme::Shuangpin) {
                    std::string c = ec.second;
                    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    if (c == res.raw_code || c.rfind(res.raw_code, 0) == 0) {
                        return true;
                    }
                } else if ((res.scheme == InputScheme::Pinyin || res.scheme == InputScheme::Shuangpin) &&
                           ec.first == InputScheme::Pinyin) {
                    std::string c = ec.second;
                    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    if (res.scheme == InputScheme::Pinyin && c == lowerCode) {
                        return true;
                    }
                    auto candSylls = ParseCustomPinyinSyllables(ec.second, m_pinyinSegmenter);
                    bool allowLastSyllablePrefix = (res.scheme == InputScheme::Pinyin &&
                                                   res.raw_code.find('\'') == std::string::npos);
                    auto qualOpt = MatchCustomPinyinCandidate(candSylls, res.syllables,
                                                              res.trailing_initial, res.is_complete,
                                                              allowLastSyllablePrefix);
                    if (qualOpt.has_value()) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    return false;
}

CoordinatorQueryResult
InputCoordinator::Query(const std::string& rawCode,
                       std::shared_ptr<const InputPolicy> policySnapshot,
                       int manualDictIndex,
                       const std::string& targetDictId) const {
    CoordinatorQueryResult result;
    result.raw_code = rawCode;

    if (!policySnapshot) {
        policySnapshot = GetPolicySnapshot();
    }
    result.policy_revision = policySnapshot->revision;

    if (rawCode.empty()) {
        result.status = CoordinatorStatus::Success;
        return result;
    }

    // Resolve input methods
    auto methods = ResolveInputMethods(rawCode, *policySnapshot);
    if (methods.empty()) {
        if (rawCode.length() > MAX_SHUANGPIN_INPUT_LENGTH) {
            result.status = CoordinatorStatus::InputLengthExceeded;
            result.error_message = "Input length exceeds maximum supported limit of " + std::to_string(MAX_SHUANGPIN_INPUT_LENGTH) + " characters";
        } else {
            result.status = CoordinatorStatus::InvalidCode;
            result.error_message = "No valid parser for input code: " + rawCode;
        }
        return result;
    }

    bool isResolutionTruncated = false;
    for (const auto& m : methods) {
        if (m.is_truncated) {
            isResolutionTruncated = true;
            break;
        }
    }
    if (isResolutionTruncated) {
        result.is_truncated = true;
        result.truncation_reason = "Ambiguous shuangpin branches exceeded limit of 16; evaluated top 16 canonical branches";
    }

    // Check dictionary health only for enabled dictionaries that are consulted
    bool needsWubi = false;
    bool needsPinyin = false;
    for (const auto& m : methods) {
        if (m.scheme == InputScheme::Wubi86) needsWubi = true;
        if (m.scheme == InputScheme::Pinyin || m.scheme == InputScheme::Shuangpin) needsPinyin = true;
    }

    if (policySnapshot->IsDictEnabled(DICT_ID_BUILTIN_WUBI) && needsWubi && m_builtinDict.WubiEntryCount() == 0) {
        result.status = CoordinatorStatus::DictLoadError;
        result.error_message = "Builtin wubi dictionary is enabled but not loaded";
        return result;
    }
    if (policySnapshot->IsDictEnabled(DICT_ID_BUILTIN_PINYIN) && needsPinyin && m_builtinDict.PinyinEntryCount() == 0) {
        result.status = CoordinatorStatus::DictLoadError;
        result.error_message = "Builtin pinyin dictionary is enabled but not loaded";
        return result;
    }

    auto enabledDicts = policySnapshot->GetEnabledDictionaryOrder();
    result.total_enabled_dicts = static_cast<int>(enabledDicts.size());

    // Determine target search scope
    std::vector<std::string> searchDicts;
    if (!targetDictId.empty()) {
        if (policySnapshot->IsDictEnabled(targetDictId)) {
            searchDicts.push_back(targetDictId);
        } else {
            result.status = CoordinatorStatus::NoCandidates;
            result.error_message = "Target dictionary is disabled or unknown: " + targetDictId;
            return result;
        }
    } else if (manualDictIndex >= 0) {
        if (manualDictIndex < static_cast<int>(enabledDicts.size())) {
            searchDicts.push_back(enabledDicts[manualDictIndex]);
        } else {
            result.status = CoordinatorStatus::NoCandidates;
            return result;
        }
    } else {
        // Automatic cascading fallback
        searchDicts = enabledDicts;
    }

    try {
        // 1. Check all enabled dictionaries for matches to populate available_dict_ids
        std::vector<std::string> dictsWithExact;
        std::vector<std::string> dictsWithPrefix;
        std::unordered_map<std::string, std::vector<Candidate>> exactCache;
        std::unordered_map<std::string, std::vector<Candidate>> prefixCache;

        for (size_t dIdx = 0; dIdx < enabledDicts.size(); ++dIdx) {
            const auto& dictId = enabledDicts[dIdx];
            int dictPriority = static_cast<int>(dIdx);
            std::vector<Candidate> exactCandidates;
            for (const auto& m : methods) {
                auto c = QuerySingleDictionary(dictId, m, true);
                for (auto& item : c) item.dict_priority = dictPriority;
                exactCandidates.insert(exactCandidates.end(), c.begin(), c.end());
            }
            if (!exactCandidates.empty()) {
                dictsWithExact.push_back(dictId);
                exactCache[dictId] = std::move(exactCandidates);
            }

            // Also check prefix matches
            std::vector<Candidate> prefixCandidates;
            for (const auto& m : methods) {
                auto c = QuerySingleDictionary(dictId, m, false);
                for (auto& item : c) item.dict_priority = dictPriority;
                prefixCandidates.insert(prefixCandidates.end(), c.begin(), c.end());
            }
            if (!prefixCandidates.empty()) {
                dictsWithPrefix.push_back(dictId);
                prefixCache[dictId] = std::move(prefixCandidates);
            }
        }

        // Available dict IDs are those with any matches
        for (const auto& dictId : enabledDicts) {
            if (std::find(dictsWithExact.begin(), dictsWithExact.end(), dictId) != dictsWithExact.end() ||
                std::find(dictsWithPrefix.begin(), dictsWithPrefix.end(), dictId) != dictsWithPrefix.end()) {
                result.available_dict_ids.push_back(dictId);
            }
        }

        // 2. Waterfall & Threshold-Aware Cascading Candidate Selection
        std::string chosenDict;
        std::vector<Candidate> chosenCandidates;
        bool isAutoCascade = (manualDictIndex < 0 && targetDictId.empty());
        uint32_t threshold = policySnapshot ? policySnapshot->fallback_candidate_threshold : 3;
        std::unordered_set<std::wstring> exactTexts;
        std::vector<std::string> selectedExactDicts;

        // Stage 1: Exact matches in searchDicts (merge custom overlay + supplement secondary if < threshold)
        for (const auto& dictId : searchDicts) {
            auto it = exactCache.find(dictId);
            if (it != exactCache.end() && !it->second.empty()) {
                if (chosenDict.empty()) {
                    chosenDict = dictId;
                }
                selectedExactDicts.push_back(dictId);
                for (const auto& ec : it->second) {
                    chosenCandidates.push_back(ec);
                    exactTexts.insert(ec.text);
                }

                if (!isAutoCascade) {
                    break;
                }
                // Custom dictionary is an overlay (never blocks builtin dictionaries).
                // For builtin dictionaries, stop cascading once exact candidates meet threshold.
                if (dictId != DICT_ID_CUSTOM && exactTexts.size() >= threshold) {
                    break;
                }
            }
        }

        // Append Wubi86 prefix completions from selected dictionaries
        if (!selectedExactDicts.empty()) {
            std::unordered_set<std::string> seenIds;
            for (const auto& ec : chosenCandidates) {
                seenIds.insert(ec.entry_id);
            }
            for (const auto& dictId : selectedExactDicts) {
                bool hasWubi = false;
                for (const auto& ec : exactCache[dictId]) {
                    if (ec.scheme == InputScheme::Wubi86) { hasWubi = true; break; }
                }
                if (hasWubi) {
                    auto itPref = prefixCache.find(dictId);
                    if (itPref != prefixCache.end() && !itPref->second.empty()) {
                        for (auto& pc : itPref->second) {
                            if (seenIds.insert(pc.entry_id).second) {
                                chosenCandidates.push_back(std::move(pc));
                            }
                        }
                    }
                }
            }
        }

        // Stage 2: Prefix completions in searchDicts (only if Stage 1 returned no exact candidates)
        if (chosenCandidates.empty()) {
            std::unordered_set<std::wstring> prefixTexts;
            for (const auto& dictId : searchDicts) {
                auto it = prefixCache.find(dictId);
                if (it != prefixCache.end() && !it->second.empty()) {
                    if (chosenDict.empty()) {
                        chosenDict = dictId;
                    }
                    for (const auto& pc : it->second) {
                        chosenCandidates.push_back(pc);
                        prefixTexts.insert(pc.text);
                    }
                    if (!isAutoCascade || (dictId != DICT_ID_CUSTOM && prefixTexts.size() >= threshold)) {
                        break;
                    }
                }
            }
        }

        if (chosenCandidates.empty()) {
            if (result.is_truncated) {
                result.status = CoordinatorStatus::BudgetTruncated;
                result.error_message = "No candidates found within evaluated branch budget (16 branches)";
            } else {
                result.status = CoordinatorStatus::NoCandidates;
            }
            return result;
        }

        result.active_dict_id = chosenDict;
        for (int i = 0; i < static_cast<int>(enabledDicts.size()); ++i) {
            if (enabledDicts[i] == chosenDict) {
                result.current_dict_index = i;
                break;
            }
        }

        // Check if there are subsequent enabled dictionaries that have matches
        result.has_next_dictionary = false;
        for (int i = result.current_dict_index + 1; i < static_cast<int>(enabledDicts.size()); ++i) {
            const auto& nextDictId = enabledDicts[i];
            if (std::find(result.available_dict_ids.begin(), result.available_dict_ids.end(), nextDictId) != result.available_dict_ids.end()) {
                result.has_next_dictionary = true;
                break;
            }
        }

        // 3. Process sort preferences and stats for chosen candidates with isolated scope
        for (auto& c : chosenCandidates) {
            // Reset prior flags to isolate scope (P1-4: prevent pin & stats leakage)
            c.is_pinned = false;
            c.order_rank = 0;
            c.selected_count = 0;
            c.effective_count = 0.0;

            std::string scopedCode = BuildScopedCode(c.scheme, c.layout_id, c.raw_code);
            auto pref = m_repo.GetSortPreference(c.scheme, scopedCode, c.entry_id);
            if (pref && pref->is_pinned) {
                c.is_pinned = true;
                c.order_rank = pref->order_rank;
            }

            // Load personal stats for candidate under its scoped code (P1-3)
            auto stat = m_repo.GetStat(c.scheme, scopedCode, c.entry_id);
            if (stat) {
                c.selected_count = stat->selected_count;
                c.last_selected_at = stat->last_selected_at;
                c.effective_count = m_repo.CalculateDecayedCount(stat->effective_count, stat->last_decay_at, m_clock->Now());
            }

            // Frequency switch control (P1-1):
            // When frequency_enabled is false, freeze frequency's influence on ordering
            if (!policySnapshot->frequency_enabled) {
                c.selected_count = 0;
                c.effective_count = 0.0;
            }
        }

        // 4. Rank using deterministic sorter with input method priority (P2-2)
        auto ranked = m_ranker.Rank(std::move(chosenCandidates));

        // 5. Deduplicate by text (preserve single source of truth for learning)
        std::vector<Candidate> deduped;
        std::unordered_set<std::wstring> seenTexts;
        for (auto& c : ranked) {
            if (seenTexts.find(c.text) == seenTexts.end()) {
                seenTexts.insert(c.text);
                deduped.push_back(std::move(c));
            }
        }

        result.candidates = std::move(deduped);
        result.status = result.candidates.empty() ? CoordinatorStatus::NoCandidates : CoordinatorStatus::Success;

        // Stage 4C-2 & Stage 6: Reverse lookup annotation
        if (policySnapshot && policySnapshot->reverse_lookup.enabled) {
            InputScheme targetScheme = policySnapshot->reverse_lookup.target_method;
            ShuangpinLayout spLayout = policySnapshot->reverse_lookup.shuangpin_layout;
            for (auto& cand : result.candidates) {
                auto rl = ReverseLookup(cand.text, targetScheme, spLayout);
                if (rl.found && !rl.primary_code.empty()) {
                    cand.reverse_lookup_code = rl.primary_code;
                    cand.is_reverse_char_by_char = !rl.is_phrase;
                }
            }
        } else if (policySnapshot && policySnapshot->mixed_input_enabled) {
            // Automatically annotate candidates recalled from secondary input methods
            // with their corresponding code in the primary input method (e.g., Wubi86 code when recalled via Pinyin/Shuangpin)
            InputScheme primaryScheme = policySnapshot->primary_method;
            ShuangpinLayout spLayout = policySnapshot->shuangpin_layout;
            size_t annotateLimit = (std::min)(result.candidates.size(), static_cast<size_t>(50));
            for (size_t i = 0; i < annotateLimit; ++i) {
                auto& cand = result.candidates[i];
                if (cand.method_priority > 0 && cand.scheme != primaryScheme && cand.scheme != InputScheme::Custom) {
                    auto rl = ReverseLookup(cand.text, primaryScheme, spLayout);
                    if (rl.found && !rl.primary_code.empty()) {
                        cand.reverse_lookup_code = rl.primary_code;
                        cand.is_reverse_char_by_char = !rl.is_phrase;
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        result.status = CoordinatorStatus::StorageError;
        result.error_message = std::string("Storage exception during query: ") + e.what();
        result.candidates.clear();
        result.active_dict_id = "";
    }

    return result;
}

CoordinatorQueryResult
InputCoordinator::QueryNextDictionary(const std::string& rawCode,
                                     int currentDictIndex,
                                     std::shared_ptr<const InputPolicy> policySnapshot) const {
    if (!policySnapshot) {
        policySnapshot = GetPolicySnapshot();
    }
    auto enabledDicts = policySnapshot->GetEnabledDictionaryOrder();
    if (enabledDicts.empty()) {
        CoordinatorQueryResult res;
        res.status = CoordinatorStatus::NoCandidates;
        return res;
    }

    // First query to find available matching dictionaries
    auto currentRes = Query(rawCode, policySnapshot, currentDictIndex);
    if (currentRes.available_dict_ids.empty()) {
        return currentRes;
    }

    // Step forward among enabledDicts to find the next dictionary that actually has matches (P2-1)
    int total = static_cast<int>(enabledDicts.size());
    int nextIndex = -1;
    for (int step = 1; step < total; ++step) {
        int candidateIdx = (currentDictIndex + step) % total;
        const auto& dictId = enabledDicts[candidateIdx];
        if (std::find(currentRes.available_dict_ids.begin(), currentRes.available_dict_ids.end(), dictId) != currentRes.available_dict_ids.end()) {
            nextIndex = candidateIdx;
            break;
        }
    }

    if (nextIndex == -1) {
        return currentRes;
    }

    return Query(rawCode, policySnapshot, nextIndex);
}

bool InputCoordinator::CommitCandidate(const Candidate& candidate,
                                       const std::string& eventId,
                                       uint64_t sessionId,
                                       uint64_t contextId,
                                       std::shared_ptr<const InputPolicy> policySnapshot) {
    if (!policySnapshot) {
        policySnapshot = GetPolicySnapshot();
    }

    // Ensure builtin entry row exists in SQLite for foreign key integrity (both when learning is on or off)
    if (candidate.source == EntrySource::Builtin || candidate.entry_id.rfind("b_", 0) == 0) {
        m_repo.EnsureBuiltinEntryExists(candidate.entry_id, candidate.text);
    }

    // Candidate affiliation verification:
    // For Shuangpin, custom words were registered under Pinyin scheme, so affiliation checks matched_code or resolved_code under Pinyin
    InputScheme affScheme = (candidate.scheme == InputScheme::Shuangpin)
        ? InputScheme::Pinyin : candidate.scheme;
    std::string affCode = !candidate.matched_code.empty() ? candidate.matched_code
        : (!candidate.resolved_code.empty() ? candidate.resolved_code : candidate.raw_code);

    // Learning scope code: scoped by layout for Shuangpin
    std::string rawOrMatched = !candidate.raw_code.empty() ? candidate.raw_code
        : (!candidate.matched_code.empty() ? candidate.matched_code : candidate.resolved_code);
    std::string learningCode = BuildScopedCode(candidate.scheme, candidate.layout_id, rawOrMatched);

    return m_repo.RecordCommit(eventId, sessionId, contextId,
                              candidate.scheme, learningCode, candidate.entry_id,
                              policySnapshot->learning_enabled,
                              affScheme, affCode);
}

bool InputCoordinator::RecordCancel(const std::string& eventId,
                                    uint64_t sessionId,
                                    uint64_t contextId) {
    return m_repo.RecordCancel(eventId, sessionId, contextId);
}

InputCoordinator::ReverseLookupResult
InputCoordinator::ReverseLookup(const std::wstring& text,
                               InputScheme targetScheme,
                               ShuangpinLayout spLayout) const {
    ReverseLookupResult result;
    if (text.empty()) return result;

    // 1. Check user custom entries in repository (highest priority)
    auto userCodes = m_repo.GetUserCodesForText(text, targetScheme);
    if (!userCodes.empty()) {
        result.found = true;
        result.is_phrase = true;
        result.primary_code = userCodes.front();
        result.all_codes = userCodes;
        return result;
    }

    // If target is Shuangpin, check if user has custom Pinyin codes that can be encoded
    if (targetScheme == InputScheme::Shuangpin) {
        auto userPyCodes = m_repo.GetUserCodesForText(text, InputScheme::Pinyin);
        if (!userPyCodes.empty()) {
            const auto& decoder = GetShuangpinDecoder(spLayout);
            std::vector<std::string> spCodes;
            std::unordered_set<std::string> seenSp;
            for (const auto& py : userPyCodes) {
                // Reuse canonical ParseCustomPinyinSyllables contract which preserves explicit
                // syllable separators (' or space) and applies longest valid syllable rule for unseparated pinyin.
                auto candSylls = ParseCustomPinyinSyllables(py, m_pinyinSegmenter);
                if (candSylls.empty()) continue;

                std::string sp = decoder.EncodePinyin(candSylls);
                if (sp.empty()) continue;

                // Validate that decoded sp roundtrips back to candSylls and preserves syllable boundaries
                auto decRes = decoder.Decode(sp);
                if (!decRes.is_valid || !decRes.is_complete) continue;
                bool matchesBranch = false;
                for (const auto& branch : decRes.branches) {
                    if (branch.syllables == candSylls) {
                        matchesBranch = true;
                        break;
                    }
                }
                if (!matchesBranch && decRes.syllables != candSylls) {
                    continue;
                }

                if (seenSp.insert(sp).second) {
                    spCodes.push_back(sp);
                }
            }
            if (!spCodes.empty()) {
                result.found = true;
                result.is_phrase = true;
                result.primary_code = spCodes.front();
                result.all_codes = spCodes;
                return result;
            }
        }
    }

    // 2. Check BuiltinDictionary for whole-word (整词) match
    if (targetScheme == InputScheme::Wubi86) {
        auto wubiCodes = m_builtinDict.ReverseLookupWubi(text);
        if (!wubiCodes.empty()) {
            result.found = true;
            result.is_phrase = true;
            result.primary_code = wubiCodes.front();
            result.all_codes = wubiCodes;
            return result;
        }
    } else if (targetScheme == InputScheme::Pinyin) {
        auto pyCodes = m_builtinDict.ReverseLookupPinyin(text);
        if (!pyCodes.empty()) {
            result.found = true;
            result.is_phrase = true;
            result.primary_code = pyCodes.front();
            result.all_codes = pyCodes;
            return result;
        }
    } else if (targetScheme == InputScheme::Shuangpin) {
        const auto& decoder = GetShuangpinDecoder(spLayout);
        auto spCodes = m_builtinDict.ReverseLookupShuangpin(text, decoder);
        if (!spCodes.empty()) {
            result.found = true;
            result.is_phrase = true;
            result.primary_code = spCodes.front();
            result.all_codes = spCodes;
            return result;
        }
    }

    // 3. Char-by-char decomposition (逐字拆码) if text has more than 1 character
    std::vector<std::wstring> chars;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c >= 0xd800 && c <= 0xdbff && (i + 1) < text.size()) {
            chars.push_back(text.substr(i, 2));
            ++i;
        } else {
            chars.push_back(text.substr(i, 1));
        }
    }

    if (chars.size() > 1) {
        std::vector<std::string> charCodes;
        bool allFound = true;
        for (const auto& ch : chars) {
            auto chRes = ReverseLookup(ch, targetScheme, spLayout);
            if (!chRes.found || chRes.primary_code.empty()) {
                allFound = false;
                break;
            }
            charCodes.push_back(chRes.primary_code);
        }

        if (allFound) {
            result.found = true;
            result.is_phrase = false;
            result.char_codes = charCodes;

            std::string joined;
            for (size_t i = 0; i < charCodes.size(); ++i) {
                if (i > 0) joined.push_back(' ');
                joined += charCodes[i];
            }
            result.primary_code = joined;
            result.all_codes = { joined };
            return result;
        }
    }

    // 4. Not found ("查不到不猜造")
    return result;
}

} // namespace ime
