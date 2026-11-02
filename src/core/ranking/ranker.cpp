#include "ranker.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <sstream>
#include <iomanip>

namespace ime {

static constexpr double RECENCY_TAU_SECONDS = 7.0 * 86400.0; // 7 days
static constexpr double WEIGHT_FREQ = 25.0;
static constexpr double WEIGHT_RECENCY = 15.0;

Ranker::Ranker(std::shared_ptr<IClock> clock) : m_clock(clock) {}

void Ranker::ComputeScore(Candidate& c, int64_t now) const {
    if (c.is_pinned) {
        c.score = 1000000.0 - static_cast<double>(c.order_rank * 1000);
        std::ostringstream oss;
        oss << "[Pinned: Rank " << c.order_rank << "]";
        c.explain_info = oss.str();
        return;
    }

    double baseScore = c.base_weight * 0.05;
    double freqBonus = WEIGHT_FREQ * std::log(1.0 + c.effective_count);

    double recencyBonus = 0.0;
    if (c.selected_count > 0 && c.last_selected_at > 0) {
        double dt = static_cast<double>((std::max)(static_cast<int64_t>(0), now - c.last_selected_at));
        recencyBonus = WEIGHT_RECENCY * std::exp(-dt / RECENCY_TAU_SECONDS);
    }

    c.score = baseScore + freqBonus + recencyBonus;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "[Quality: " << MatchQualityToString(c.quality)
        << " | Base: " << baseScore
        << " | EffCount: " << c.effective_count
        << " (FreqBonus: +" << freqBonus << ")"
        << " | Recency: +" << recencyBonus
        << " | Total: " << c.score << "]";
    c.explain_info = oss.str();
}

std::vector<Candidate> Ranker::Rank(std::vector<Candidate> candidates) const {
    if (candidates.empty()) return {};

    int64_t now = m_clock->Now();

    // 1. Deduplicate by phrase text
    // If both builtin and user exist for the same text, merge them into the user representation
    std::unordered_map<std::wstring, size_t> textMap;
    std::vector<Candidate> uniqueCandidates;

    for (auto& c : candidates) {
        auto it = textMap.find(c.text);
        if (it == textMap.end()) {
            textMap[c.text] = uniqueCandidates.size();
            uniqueCandidates.push_back(std::move(c));
        } else {
            auto& existing = uniqueCandidates[it->second];
            // Merge rules:
            // User source overrides builtin
            if (c.source == EntrySource::User && existing.source == EntrySource::Builtin) {
                existing.entry_id = c.entry_id;
                existing.source = EntrySource::User;
            }
            // Pinned overrides unpinned
            if (c.is_pinned && !existing.is_pinned) {
                existing.is_pinned = true;
                existing.order_rank = c.order_rank;
            }
            // Retain maximum stats without duplicating across code aliases
            existing.selected_count = (std::max)(existing.selected_count, c.selected_count);
            existing.effective_count = (std::max)(existing.effective_count, c.effective_count);
            existing.last_selected_at = (std::max)(existing.last_selected_at, c.last_selected_at);
            // Better match quality takes precedence
            if (static_cast<int>(c.quality) < static_cast<int>(existing.quality)) {
                existing.quality = c.quality;
                existing.matched_code = c.matched_code;
            }
            existing.base_weight = (std::max)(existing.base_weight, c.base_weight);
        }
    }

    // 2. Compute score & explain info
    for (auto& c : uniqueCandidates) {
        ComputeScore(c, now);
    }

    // 3. Strict 4-Level Deterministic Sorter
    std::sort(uniqueCandidates.begin(), uniqueCandidates.end(), [](const Candidate& a, const Candidate& b) {
        // Level 1: Pinned status
        if (a.is_pinned != b.is_pinned) {
            return a.is_pinned > b.is_pinned; // Pinned comes first
        }
        if (a.is_pinned && b.is_pinned) {
            if (a.order_rank != b.order_rank) {
                return a.order_rank < b.order_rank; // Lower rank number comes first
            }
        }

        // Level 2: Match Quality group
        if (a.quality != b.quality) {
            return static_cast<int>(a.quality) < static_cast<int>(b.quality); // ExactFull < PrefixFull < PrefixCompletion
        }

        // Level 3: Score (DESC)
        if (std::abs(a.score - b.score) > 1e-6) {
            return a.score > b.score;
        }

        // Level 4: Stable entry_id ASC (tie-breaker)
        return a.entry_id < b.entry_id;
    });

    return uniqueCandidates;
}

} // namespace ime

