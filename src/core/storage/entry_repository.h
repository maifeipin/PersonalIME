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
    bool is_pinned = false;
    int order_rank = 0;
};

struct EntryStat {
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
    bool DeleteEntry(const std::string& entryId);
    std::optional<Entry> GetEntry(const std::string& entryId);
    std::vector<Entry> GetAllUserEntries();

    // Query User Entries by scheme + code (exact or prefix)
    std::vector<Candidate> QueryUserCandidates(InputScheme scheme, const std::string& code);

    // Sort Preferences (Pin / Rank)
    bool SetPinPreference(InputScheme scheme, const std::string& code, const std::string& entryId, bool isPinned, int orderRank = 1);
    bool UnpinPreference(InputScheme scheme, const std::string& code, const std::string& entryId);
    std::optional<SortPreference> GetSortPreference(InputScheme scheme, const std::string& code, const std::string& entryId);

    // Stats & Learning
    bool RecordCommit(const std::string& eventId, InputScheme scheme, const std::string& code, const std::string& entryId);
    std::optional<EntryStat> GetStat(InputScheme scheme, const std::string& code, const std::string& entryId);
    bool ResetStats(const std::string& entryId = "");

    // Utility: ensure a builtin entry has a row in entries if needed for stats/pinning
    void EnsureBuiltinEntryExists(const std::string& entryId, const std::wstring& text);

private:
    SqliteDb& m_db;
    std::shared_ptr<IClock> m_clock;

    std::string GenerateUniqueId();
    double CalculateDecayedCount(double currentCount, int64_t lastDecayAt, int64_t now) const;
};

} // namespace ime

