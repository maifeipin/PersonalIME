#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include "../types.h"
#include "input_policy.h"
#include "../encoding/shuangpin_decoder.h"
#include "../encoding/pinyin_segmenter.h"
#include "../dictionary/builtin_dict.h"
#include "../storage/entry_repository.h"
#include "../ranking/ranker.h"

namespace ime {

enum class CoordinatorStatus {
    Success = 0,
    NoCandidates = 1,
    StorageError = 2,
    DictLoadError = 3,
    InvalidCode = 4,
    BudgetTruncated = 5,
    InputLengthExceeded = 6
};

struct CoordinatorQueryResult {
    CoordinatorStatus status = CoordinatorStatus::Success;
    std::string error_message;
    std::string raw_code;
    uint64_t policy_revision = 0;

    // Truncation & budget metadata (P1-2)
    bool is_truncated = false;
    std::string truncation_reason;

    // Ordered candidates for display/selection
    std::vector<Candidate> candidates;

    // Dictionary provenance and manual fallback navigation
    std::string active_dict_id;            // Dictionary that provided the primary candidate set
    std::vector<std::string> available_dict_ids; // All enabled dicts that have matches for this query
    bool has_next_dictionary = false;      // True if a subsequent dictionary has candidates
    int current_dict_index = 0;            // 0-based index of active_dict_id in enabled dictionary order
    int total_enabled_dicts = 0;
};

class InputCoordinator {
public:
    InputCoordinator(EntryRepository& repo,
                     const BuiltinDictionary& builtinDict,
                     std::shared_ptr<IClock> clock = nullptr);

    // Policy Snapshot Management
    std::shared_ptr<const InputPolicy> GetPolicySnapshot() const;
    void SetPolicy(const InputPolicy& policy);
    void UpdatePolicy(const InputPolicy& policy);

    // Primary Query Interface with Cascading Fallback & Manual Override
    // - policySnapshot: if null, uses current policy snapshot (for composition session locking)
    // - manualDictIndex: if -1, automatic cascading fallback; if >= 0, explicitly query that dictionary
    // - targetDictId: if non-empty, explicitly query only this dictionary
    CoordinatorQueryResult Query(const std::string& rawCode,
                                 std::shared_ptr<const InputPolicy> policySnapshot = nullptr,
                                 int manualDictIndex = -1,
                                 const std::string& targetDictId = "") const;

    // Manual Fallback: Step to the next dictionary that has candidates
    CoordinatorQueryResult QueryNextDictionary(const std::string& rawCode,
                                              int currentDictIndex,
                                              std::shared_ptr<const InputPolicy> policySnapshot = nullptr) const;

    // Commit Candidate with provenance and learning switch respect
    bool CommitCandidate(const Candidate& candidate,
                         const std::string& eventId,
                         uint64_t sessionId = 0,
                         uint64_t contextId = 0,
                         std::shared_ptr<const InputPolicy> policySnapshot = nullptr);

    // Cancel / Rollback recording
    bool RecordCancel(const std::string& eventId,
                      uint64_t sessionId = 0,
                      uint64_t contextId = 0);

    // Pinning and manual ordering with strict scope (scheme + layout_id + query_code + entry_id)
    bool SetPinPreference(InputScheme scheme,
                          const std::string& code,
                          const std::string& entryId,
                          bool isPinned,
                          int orderRank = 1,
                          const std::string& layoutId = "",
                          std::optional<uint64_t> expectedRevision = std::nullopt,
                          uint64_t* outRevision = nullptr,
                          bool* outConflict = nullptr);
    bool UnpinPreference(InputScheme scheme,
                         const std::string& code,
                         const std::string& entryId,
                         const std::string& layoutId = "",
                         std::optional<uint64_t> expectedRevision = std::nullopt,
                         uint64_t* outRevision = nullptr,
                         bool* outConflict = nullptr);

    // Utility: build scoped code key for storage
    static std::string BuildScopedCode(InputScheme scheme, const std::string& layoutId, const std::string& code);

    // Query candidates belonging to a specific input scope (for management/preview)
    std::vector<Candidate> QueryScopedCandidates(InputScheme scheme,
                                                const std::string& layoutId,
                                                const std::string& code,
                                                const std::string& keyword = "") const;

    // Check if an entry ID is valid for the target query scope
    bool IsEntryValidForScope(InputScheme scheme,
                              const std::string& layoutId,
                              const std::string& code,
                              const std::string& entryId) const;

    // Stage 4C-2: Reverse lookup code for given text and target scheme
    struct ReverseLookupResult {
        bool found = false;
        bool is_phrase = false;             // true if whole-word, false if char-by-char decomposition
        std::string primary_code;           // Primary code for display (e.g. "aaaa" or "gong")
        std::vector<std::string> all_codes; // All valid codes
        std::vector<std::string> char_codes;// If char-by-char decomposition, codes for each character
    };

    ReverseLookupResult ReverseLookup(const std::wstring& text,
                                      InputScheme targetScheme,
                                      ShuangpinLayout spLayout = ShuangpinLayout::Natural) const;

private:
    EntryRepository& m_repo;
    const BuiltinDictionary& m_builtinDict;
    std::shared_ptr<IClock> m_clock;
    Ranker m_ranker;
    PinyinSegmenter m_pinyinSegmenter;
    ShuangpinDecoder m_decoderNatural;
    ShuangpinDecoder m_decoderMS;
    ShuangpinDecoder m_decoderXiaohe;

    const ShuangpinDecoder& GetShuangpinDecoder(ShuangpinLayout layout) const;

    mutable std::mutex m_policyMutex;
    std::shared_ptr<const InputPolicy> m_currentPolicy;

    struct MethodResolution {
        InputScheme scheme = InputScheme::Wubi86;
        std::string raw_code;
        std::string resolved_code;
        std::string spaced_code;
        std::string layout_id;
        std::vector<std::string> syllables;
        std::string trailing_initial;
        int method_priority = 0;
        bool is_valid = false;
        bool is_complete = true;
        bool is_truncated = false;
    };

    std::vector<MethodResolution> ResolveInputMethods(const std::string& rawCode,
                                                      const InputPolicy& policy) const;

    std::vector<Candidate> QuerySingleDictionary(const std::string& dictId,
                                                const MethodResolution& res,
                                                bool exactOnly) const;
};

} // namespace ime

