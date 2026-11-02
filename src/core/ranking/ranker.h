#pragma once

#include "../types.h"
#include "../clock.h"
#include <vector>
#include <memory>

namespace ime {

class Ranker {
public:
    explicit Ranker(std::shared_ptr<IClock> clock);

    // Merges duplicates, computes scores, and sorts according to the 4-level deterministic rules
    std::vector<Candidate> Rank(std::vector<Candidate> candidates) const;

private:
    std::shared_ptr<IClock> m_clock;

    void ComputeScore(Candidate& c, int64_t now) const;
};

} // namespace ime

