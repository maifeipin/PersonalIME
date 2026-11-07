#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "../types.h"
#include "../dictionary/builtin_dict.h"
#include "../storage/entry_repository.h"

namespace ime {

struct DecodedSentence {
    std::wstring text;
    double score = 0.0;
    std::vector<std::pair<size_t, size_t>> word_ranges; // [start_syl, end_syl)
    std::vector<std::wstring> words;
    std::vector<Candidate> sub_candidates; // Local sub-words for partial commit
};

class SentenceDecoder {
public:
    static double GetCoreWordBoost(const std::wstring& word) {
        // Ultra high frequency functional and grammatical words from Modern Chinese corpus
        // Compensates for dictionary frequency capping (min(1000, raw_weight))
        static const std::unordered_map<std::wstring, double> s_coreBoosts = {
            {L"的", 5.0}, {L"了", 4.0}, {L"我", 4.0}, {L"是", 4.0}, {L"在", 3.5},
            {L"你", 3.5}, {L"和", 3.5}, {L"有", 3.5}, {L"不", 3.5}, {L"就", 3.5},
            {L"一个", 3.5}, {L"也", 3.0}, {L"都", 3.0}, {L"要", 3.0}, {L"好", 3.0},
            {L"他", 3.0}, {L"说", 3.0}, {L"去", 3.0}, {L"没有", 3.0}, {L"我们", 3.0},
            {L"可以", 3.0}, {L"到", 2.5}, {L"什么", 2.5}, {L"没", 2.5}, {L"上", 2.5},
            {L"对", 2.5}, {L"还", 2.5}, {L"那", 2.5}, {L"这个", 2.5}, {L"人", 2.5},
            {L"现在", 2.5}, {L"中", 2.5}, {L"自己", 2.5}, {L"来", 2.5}, {L"会", 2.5},
            {L"她", 2.5}, {L"这", 2.5}, {L"不是", 2.5}, {L"就是", 2.5}, {L"很", 2.5}
        };
        auto it = s_coreBoosts.find(word);
        return (it != s_coreBoosts.end()) ? it->second : 0.0;
    }

    static std::optional<DecodedSentence> DecodeSentence(
        const std::vector<std::string>& syllables,
        const BuiltinDictionary& builtinDict,
        EntryRepository& repo,
        std::shared_ptr<IClock> clock = nullptr)
    {
        size_t N = syllables.size();
        if (N < 2) return std::nullopt;

        // DP state at node j (from 0 to N)
        struct DpNode {
            double score = -1e9;
            int prev_idx = -1;
            std::wstring word;
            double word_weight = 0.0;
        };

        std::vector<DpNode> dp(N + 1);
        dp[0].score = 0.0;

        int64_t nowTime = clock ? clock->Now() : 0;

        // Collect all evaluated edges for generating sub-candidates
        std::vector<Candidate> prefixWordCandidates;
        std::unordered_set<std::wstring> seenPrefixWords;

        for (size_t j = 1; j <= N; ++j) {
            size_t minI = (j > 8) ? (j - 8) : 0;
            for (size_t i = minI; i < j; ++i) {
                if (dp[i].score < -1e8) continue;

                // Build spaced syllables for [i, j)
                std::string spaced;
                for (size_t k = i; k < j; ++k) {
                    if (k > i) spaced += " ";
                    spaced += syllables[k];
                }

                // 1. Query Builtin dictionary
                const auto* bItems = builtinDict.FindPinyinExactSpaced(spaced);

                // 2. Query User dictionary
                auto uCands = repo.QueryUserCandidates(InputScheme::Pinyin, spaced);

                // Best word for this edge [i, j)
                std::wstring bestWord;
                double bestEdgeScore = -1e9;
                double bestWeight = 0.0;

                auto evaluateWord = [&](const std::wstring& text, double baseWeight, const std::string& entryId) {
                    double effectiveCount = 0.0;
                    if (!entryId.empty()) {
                        auto stat = repo.GetStat(InputScheme::Pinyin, spaced, entryId);
                        if (stat) {
                            effectiveCount = repo.CalculateDecayedCount(stat->effective_count, stat->last_decay_at, nowTime);
                        }
                    }

                    // Normalize unigram frequency to log probability:
                    // Using total corpus log probability (~13.8) ensures word probabilities are negative,
                    // naturally penalizing fragmentation into single characters (least-words Viterbi principle)
                    static constexpr double LOG_TOTAL_FREQ = 13.8;
                    double logProb = std::log(baseWeight + 1.0) - LOG_TOTAL_FREQ;
                    double userBoost = (effectiveCount > 0.0) ? (std::log(effectiveCount + 1.0) * 3.0) : 0.0;
                    double lengthBonus = static_cast<double>(j - i - 1) * 4.0; // strongly favor longer phrase matches
                    double coreBoost = GetCoreWordBoost(text);
                    double wordScore = logProb + userBoost + lengthBonus + coreBoost;

                    if (wordScore >= bestEdgeScore) {
                        bestEdgeScore = wordScore;
                        bestWord = text;
                        bestWeight = baseWeight;
                    }

                    // If edge starts from index 0, add to prefix word candidates
                    if (i == 0 && seenPrefixWords.find(text) == seenPrefixWords.end()) {
                        seenPrefixWords.insert(text);
                        Candidate sc;
                        sc.entry_id = "sub_" + entryId;
                        sc.text = text;
                        sc.scheme = InputScheme::Pinyin;
                        sc.matched_code = spaced;
                        sc.quality = MatchQuality::ExactFull;
                        sc.score = 5000.0 + wordScore;
                        sc.dict_id = entryId.rfind("b_", 0) == 0 ? DICT_ID_BUILTIN_PINYIN : DICT_ID_CUSTOM;
                        prefixWordCandidates.push_back(std::move(sc));
                    }
                };

                // Evaluate custom entries first (higher personal relevance)
                for (const auto& uc : uCands) {
                    if (uc.quality == MatchQuality::ExactFull) {
                        evaluateWord(uc.text, uc.base_weight + 500.0, uc.entry_id);
                    }
                }

                if (bItems) {
                    for (const auto& item : *bItems) {
                        evaluateWord(item.text, item.weight, item.id);
                    }
                }

                if (!bestWord.empty() && (dp[i].score + bestEdgeScore >= dp[j].score)) {
                    dp[j].score = dp[i].score + bestEdgeScore;
                    dp[j].prev_idx = static_cast<int>(i);
                    dp[j].word = bestWord;
                    dp[j].word_weight = bestWeight;
                }
            }
        }

        if (dp[N].prev_idx == -1) {
            return std::nullopt;
        }

        // Backtrack optimal path
        std::vector<std::wstring> words;
        std::vector<std::pair<size_t, size_t>> ranges;
        int curr = static_cast<int>(N);
        while (curr > 0) {
            int p = dp[curr].prev_idx;
            if (p < 0) break;
            words.push_back(dp[curr].word);
            ranges.push_back({static_cast<size_t>(p), static_cast<size_t>(curr)});
            curr = p;
        }

        std::reverse(words.begin(), words.end());
        std::reverse(ranges.begin(), ranges.end());

        DecodedSentence result;
        for (const auto& w : words) {
            result.text += w;
        }
        result.score = dp[N].score;
        result.words = words;
        result.word_ranges = ranges;
        result.sub_candidates = std::move(prefixWordCandidates);

        return result;
    }
};

} // namespace ime
