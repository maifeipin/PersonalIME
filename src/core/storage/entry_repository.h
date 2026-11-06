#pragma once

#include "sqlite_db.h"
#include "../types.h"
#include "../clock.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace ime {

class BuiltinDictionary;

struct SortPreference {
    InputScheme scheme = InputScheme::Pinyin;
    std::string code;
    std::string entry_id;
    bool is_pinned = false;
    int order_rank = 0;
};

struct EntryStat {
    InputScheme scheme = InputScheme::Pinyin;
    std::string code;
    std::string entry_id;
    int selected_count = 0;
    int64_t last_selected_at = 0;
    double effective_count = 0.0;
    int64_t last_decay_at = 0;
};

class EntryRepository {
public:
    explicit EntryRepository(SqliteDb& db, std::shared_ptr<IClock> clock);

    // CRUD
    std::string AddEntry(const std::wstring& text,
                         const std::vector<std::pair<InputScheme, std::string>>& codes,
                         EntrySource source = EntrySource::User,
                         const std::string& customId = "");
    bool UpdateEntryText(const std::string& entryId, const std::wstring& newText);
    bool AddEntryCodes(const std::string& entryId, const std::vector<std::pair<InputScheme, std::string>>& codes);
    bool SetEntryCodes(const std::string& entryId, const std::vector<std::pair<InputScheme, std::string>>& codes);
    bool UpdateEntry(const std::string& entryId, const std::wstring& newText, const std::vector<std::pair<InputScheme, std::string>>& newCodes);
    bool DeleteEntry(const std::string& entryId);
    std::optional<Entry> GetEntry(const std::string& entryId);
    std::vector<Entry> GetAllUserEntries();
    std::vector<Entry> GetAllEntries();

    // Query User Entries by scheme + code (exact or prefix)
    std::vector<Candidate> QueryUserCandidates(InputScheme scheme, const std::string& code);

    // Text-to-code reverse lookup for user entries (Stage 4C-2)
    std::vector<std::string> GetUserCodesForText(const std::wstring& text, InputScheme scheme = InputScheme::Unknown) const;

    // Verification
    bool VerifyCandidateAffiliation(InputScheme scheme, const std::string& code, const std::string& entryId);
    bool HasCommitEvent(const std::string& eventId);

    struct CommitEventRecord {
        std::string event_id;
        uint64_t session_id = 0;
        uint64_t context_id = 0;
        int status = 0; // 0 = not found, 1 = Recorded, 2 = Cancelled
        int64_t committed_at = 0;
    };
    CommitEventRecord GetCommitEventRecord(const std::string& eventId);

    // Sort Preferences (Pin / Rank)
    bool SetPinPreference(InputScheme scheme, const std::string& code, const std::string& entryId, bool isPinned, int orderRank = 1,
                          std::optional<uint64_t> expectedRevision = std::nullopt, uint64_t* outRevision = nullptr, bool* outConflict = nullptr);
    bool UnpinPreference(InputScheme scheme, const std::string& code, const std::string& entryId,
                         std::optional<uint64_t> expectedRevision = std::nullopt, uint64_t* outRevision = nullptr, bool* outConflict = nullptr);
    std::optional<SortPreference> GetSortPreference(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::vector<SortPreference> GetAllSortPreferences();
    std::vector<SortPreference> GetPinnedPreferences(InputScheme scheme, const std::string& code);

    // Stats & Learning
    double CalculateDecayedCount(double currentCount, int64_t lastDecayAt, int64_t now) const;
    bool RecordCommit(const std::string& eventId, uint64_t sessionId, uint64_t contextId,
                      InputScheme scheme, const std::string& code, const std::string& entryId,
                      bool updateStats = true,
                      InputScheme affiliationScheme = InputScheme::Unknown,
                      const std::string& affiliationCode = "");
    bool RecordCommit(const std::string& eventId, InputScheme scheme, const std::string& code, const std::string& entryId,
                      bool updateStats = true) {
        return RecordCommit(eventId, 0, 0, scheme, code, entryId, updateStats);
    }
    bool RecordCancel(const std::string& eventId, uint64_t sessionId, uint64_t contextId);
    bool SetStatDirect(InputScheme scheme, const std::string& code, const std::string& entryId,
                       int count, int64_t lastUsed, double effCount, int64_t lastDecayAt, bool mergeMode = false);
    std::optional<EntryStat> GetStat(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::vector<EntryStat> GetAllEntryStats();
    std::vector<EntryStat> GetActiveStats(InputScheme scheme, const std::string& code);
    bool ResetStats(const std::string& entryId = "");
    bool ResetAllStats();
    int64_t GetLastResetStatsAt();
    void SetLastResetStatsAt(int64_t ts);
    int64_t GetGlobalStatsResetEpoch();
    int64_t GetEntryStatsResetEpoch(const std::string& entryId);

    // Reorder pinned entries atomically
    bool ReorderPinnedEntries(InputScheme scheme, const std::string& code, const std::vector<std::string>& entryIds,
                             std::optional<uint64_t> expectedRevision = std::nullopt, uint64_t* outRevision = nullptr, bool* outConflict = nullptr);
    uint64_t GetPinnedRevision(InputScheme scheme, const std::string& code);
    bool SetPinnedRevision(InputScheme scheme, const std::string& code, uint64_t revision);

    // Meta Settings Persistence
    bool SetMetaSetting(const std::string& key, const std::string& value);
    std::optional<std::string> GetMetaSetting(const std::string& key);

    // Pending commits checks
    bool HasPendingCommitsForEntry(const std::string& entryId);
    bool HasAnyPendingCommits();

    // Utility: ensure a builtin entry has a row in entries if needed for stats/pinning
    void SetBuiltinDictionary(const BuiltinDictionary* dict) { m_builtinDict = dict; }
    bool EnsureBuiltinEntryExists(const std::string& entryId, const std::wstring& text);

    // Persistent Pending Commits across service crashes / restarts
    struct PendingCommitRecord {
        std::string event_id;
        uint64_t session_id = 0;
        uint64_t context_id = 0;
        InputScheme scheme = InputScheme::Unknown;
        std::string query_code;
        std::string entry_id;
        std::wstring text;
        int64_t created_at = 0;
        bool learning_enabled = true;
        std::string layout_id;
        std::string matched_code;
        std::string resolved_code;
        int64_t global_epoch = 0;
        int64_t entry_epoch = 0;
    };
    bool SavePendingCommit(const PendingCommitRecord& rec);
    bool RemovePendingCommit(const std::string& eventId);
    bool LoadAllPendingCommits(std::vector<PendingCommitRecord>& outRecords);
    std::vector<PendingCommitRecord> LoadAllPendingCommits() {
        std::vector<PendingCommitRecord> recs;
        LoadAllPendingCommits(recs);
        return recs;
    }

private:
    SqliteDb& m_db;
    std::shared_ptr<IClock> m_clock;
    const BuiltinDictionary* m_builtinDict = nullptr;

    std::string GenerateUniqueId();
};

} // namespace ime

