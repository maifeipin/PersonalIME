#pragma once

#include "sqlite_db.h"
#include "../types.h"
#include "../clock.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace ime {

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

    // Verification
    bool VerifyCandidateAffiliation(InputScheme scheme, const std::string& code, const std::string& entryId);

    // Sort Preferences (Pin / Rank)
    bool SetPinPreference(InputScheme scheme, const std::string& code, const std::string& entryId, bool isPinned, int orderRank = 1);
    bool UnpinPreference(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::optional<SortPreference> GetSortPreference(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::vector<SortPreference> GetAllSortPreferences();
    std::vector<SortPreference> GetPinnedPreferences(InputScheme scheme, const std::string& code);

    // Stats & Learning
    bool RecordCommit(const std::string& eventId, InputScheme scheme, const std::string& code, const std::string& entryId);
    bool SetStatDirect(InputScheme scheme, const std::string& code, const std::string& entryId,
                       int count, int64_t lastUsed, double effCount, int64_t lastDecayAt, bool mergeMode = false);
    std::optional<EntryStat> GetStat(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::vector<EntryStat> GetAllEntryStats();
    std::vector<EntryStat> GetActiveStats(InputScheme scheme, const std::string& code);
    bool ResetStats(const std::string& entryId = "");

    // Utility: ensure a builtin entry has a row in entries if needed for stats/pinning
    bool EnsureBuiltinEntryExists(const std::string& entryId, const std::wstring& text);

private:
    SqliteDb& m_db;
    std::shared_ptr<IClock> m_clock;

    std::string GenerateUniqueId();
    double CalculateDecayedCount(double currentCount, int64_t lastDecayAt, int64_t now) const;
};

} // namespace ime

