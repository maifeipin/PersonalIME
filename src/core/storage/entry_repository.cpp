#include "entry_repository.h"
#include <cmath>
#include <sstream>
#include <random>
#include <iomanip>
#include <unordered_map>

namespace ime {

static constexpr double HALF_LIFE_SECONDS = 30.0 * 86400.0; // 30 days

EntryRepository::EntryRepository(SqliteDb& db, std::shared_ptr<IClock> clock)
    : m_db(db), m_clock(clock) {}

std::string EntryRepository::GenerateUniqueId() {
    static std::mt19937_64 rng(123456789);
    uint64_t val = rng();
    int64_t now = m_clock->Now();
    std::ostringstream oss;
    oss << "u_" << std::hex << now << "_" << std::setfill('0') << std::setw(8) << (val & 0xFFFFFFFF);
    return oss.str();
}

double EntryRepository::CalculateDecayedCount(double currentCount, int64_t lastDecayAt, int64_t now) const {
    if (lastDecayAt <= 0 || now <= lastDecayAt || currentCount <= 0.0) {
        return currentCount;
    }
    double dt = static_cast<double>(now - lastDecayAt);
    double factor = std::pow(2.0, -dt / HALF_LIFE_SECONDS);
    return currentCount * factor;
}

std::string EntryRepository::AddEntry(const std::wstring& text,
                                     const std::vector<std::pair<InputScheme, std::string>>& codes,
                                     EntrySource source,
                                     const std::string& customId) {
    if (!IsValidText(text)) return "";

    std::vector<std::pair<InputScheme, std::string>> validCodes;
    for (const auto& [scheme, code] : codes) {
        if (scheme == InputScheme::Unknown) continue;
        std::string lowerCode = code;
        std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!IsValidCode(scheme, lowerCode)) continue;
        validCodes.emplace_back(scheme, lowerCode);
    }
    if (validCodes.empty()) return "";

    std::string id = customId.empty() ? GenerateUniqueId() : customId;
    int64_t now = m_clock->Now();
    std::string textUtf8 = WideToUtf8(text);

    SqliteDb::Transaction tx(m_db);

    auto stmtEntry = m_db.Prepare("INSERT INTO entries (id, text, source, created_at, updated_at) VALUES (?, ?, ?, ?, ?);");
    if (!stmtEntry) return "";
    stmtEntry->BindText(1, id);
    stmtEntry->BindText(2, textUtf8);
    stmtEntry->BindInt(3, static_cast<int>(source));
    stmtEntry->BindInt64(4, now);
    stmtEntry->BindInt64(5, now);
    if (!stmtEntry->Execute() && m_db.Changes() == 0) {
        tx.Rollback();
        return "";
    }

    auto stmtCode = m_db.Prepare("INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at) VALUES (?, ?, ?, ?);");
    if (!stmtCode) {
        tx.Rollback();
        return "";
    }

    for (const auto& [scheme, lowerCode] : validCodes) {
        stmtCode->Reset();
        stmtCode->BindText(1, id);
        stmtCode->BindInt(2, static_cast<int>(scheme));
        stmtCode->BindText(3, lowerCode);
        stmtCode->BindInt64(4, now);
        stmtCode->Execute();
    }

    if (!tx.Commit()) {
        return "";
    }
    return id;
}

bool EntryRepository::AddEntryCodes(const std::string& entryId, const std::vector<std::pair<InputScheme, std::string>>& codes) {
    if (entryId.empty() || codes.empty()) return false;
    int64_t now = m_clock->Now();
    SqliteDb::Transaction tx(m_db);

    auto stmtCode = m_db.Prepare("INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at) VALUES (?, ?, ?, ?);");
    if (!stmtCode) return false;

    for (const auto& [scheme, code] : codes) {
        if (scheme == InputScheme::Unknown) continue;
        std::string lowerCode = code;
        std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!IsValidCode(scheme, lowerCode)) continue;

        stmtCode->Reset();
        stmtCode->BindText(1, entryId);
        stmtCode->BindInt(2, static_cast<int>(scheme));
        stmtCode->BindText(3, lowerCode);
        stmtCode->BindInt64(4, now);
        stmtCode->Execute();
    }

    return tx.Commit();
}

bool EntryRepository::UpdateEntryText(const std::string& entryId, const std::wstring& newText) {
    if (entryId.empty() || newText.empty()) return false;
    int64_t now = m_clock->Now();
    std::string textUtf8 = WideToUtf8(newText);

    auto stmt = m_db.Prepare("UPDATE entries SET text = ?, updated_at = ? WHERE id = ?;");
    if (!stmt) return false;
    stmt->BindText(1, textUtf8);
    stmt->BindInt64(2, now);
    stmt->BindText(3, entryId);
    stmt->Step();
    return (m_db.Changes() > 0);
}

bool EntryRepository::DeleteEntry(const std::string& entryId) {
    if (entryId.empty()) return false;
    SqliteDb::Transaction tx(m_db);

    auto stmt = m_db.Prepare("DELETE FROM entries WHERE id = ?;");
    if (!stmt) return false;
    stmt->BindText(1, entryId);
    stmt->Step();
    bool ok = (m_db.Changes() > 0);

    if (ok) {
        // Cascade manually if foreign keys are disabled
        auto s1 = m_db.Prepare("DELETE FROM entry_codes WHERE entry_id = ?;");
        if (s1) { s1->BindText(1, entryId); s1->Step(); }
        auto s2 = m_db.Prepare("DELETE FROM sort_preferences WHERE entry_id = ?;");
        if (s2) { s2->BindText(1, entryId); s2->Step(); }
        auto s3 = m_db.Prepare("DELETE FROM entry_stats WHERE entry_id = ?;");
        if (s3) { s3->BindText(1, entryId); s3->Step(); }
        tx.Commit();
    } else {
        tx.Rollback();
    }
    return ok;
}

std::optional<Entry> EntryRepository::GetEntry(const std::string& entryId) {
    auto stmt = m_db.Prepare("SELECT id, text, source, created_at, updated_at FROM entries WHERE id = ?;");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, entryId);
    if (!stmt->Step()) return std::nullopt;

    Entry e;
    e.id = stmt->ColumnText(0);
    e.text = Utf8ToWide(stmt->ColumnText(1));
    e.source = static_cast<EntrySource>(stmt->ColumnInt(2));
    e.created_at = stmt->ColumnInt64(3);
    e.updated_at = stmt->ColumnInt64(4);

    auto stmtCodes = m_db.Prepare("SELECT scheme, code FROM entry_codes WHERE entry_id = ?;");
    if (stmtCodes) {
        stmtCodes->BindText(1, entryId);
        while (stmtCodes->Step()) {
            InputScheme sch = static_cast<InputScheme>(stmtCodes->ColumnInt(0));
            std::string code = stmtCodes->ColumnText(1);
            e.codes.emplace_back(sch, code);
        }
    }
    return e;
}

std::vector<Entry> EntryRepository::GetAllUserEntries() {
    std::vector<Entry> list;
    auto stmt = m_db.Prepare("SELECT id, text, source, created_at, updated_at FROM entries WHERE source = 1;");
    if (!stmt) return list;

    while (stmt->Step()) {
        Entry e;
        e.id = stmt->ColumnText(0);
        e.text = Utf8ToWide(stmt->ColumnText(1));
        e.source = static_cast<EntrySource>(stmt->ColumnInt(2));
        e.created_at = stmt->ColumnInt64(3);
        e.updated_at = stmt->ColumnInt64(4);
        list.push_back(std::move(e));
    }

    auto stmtCodes = m_db.Prepare("SELECT scheme, code FROM entry_codes WHERE entry_id = ?;");
    for (auto& e : list) {
        if (stmtCodes) {
            stmtCodes->Reset();
            stmtCodes->BindText(1, e.id);
            while (stmtCodes->Step()) {
                InputScheme sch = static_cast<InputScheme>(stmtCodes->ColumnInt(0));
                std::string code = stmtCodes->ColumnText(1);
                e.codes.emplace_back(sch, code);
            }
        }
    }
    return list;
}

std::vector<Entry> EntryRepository::GetAllEntries() {
    std::vector<Entry> list;
    auto stmt = m_db.Prepare("SELECT id, text, source, created_at, updated_at FROM entries;");
    if (!stmt) return list;

    while (stmt->Step()) {
        Entry e;
        e.id = stmt->ColumnText(0);
        e.text = Utf8ToWide(stmt->ColumnText(1));
        e.source = static_cast<EntrySource>(stmt->ColumnInt(2));
        e.created_at = stmt->ColumnInt64(3);
        e.updated_at = stmt->ColumnInt64(4);
        list.push_back(std::move(e));
    }

    auto stmtCodes = m_db.Prepare("SELECT scheme, code FROM entry_codes WHERE entry_id = ?;");
    auto stmtMetaCodes = m_db.Prepare(
        "SELECT scheme, code FROM sort_preferences WHERE entry_id = ? "
        "UNION "
        "SELECT scheme, code FROM entry_stats WHERE entry_id = ?;"
    );

    for (auto& e : list) {
        if (stmtCodes) {
            stmtCodes->Reset();
            stmtCodes->BindText(1, e.id);
            while (stmtCodes->Step()) {
                InputScheme sch = static_cast<InputScheme>(stmtCodes->ColumnInt(0));
                std::string code = stmtCodes->ColumnText(1);
                e.codes.emplace_back(sch, code);
            }
        }
        if (e.codes.empty() && stmtMetaCodes) {
            stmtMetaCodes->Reset();
            stmtMetaCodes->BindText(1, e.id);
            stmtMetaCodes->BindText(2, e.id);
            while (stmtMetaCodes->Step()) {
                InputScheme sch = static_cast<InputScheme>(stmtMetaCodes->ColumnInt(0));
                std::string code = stmtMetaCodes->ColumnText(1);
                e.codes.emplace_back(sch, code);
            }
        }
    }
    return list;
}

std::vector<Candidate> EntryRepository::QueryUserCandidates(InputScheme scheme, const std::string& code) {
    std::vector<Candidate> result;
    if (code.empty()) return result;

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string escapedLike = EscapeLike(lowerCode) + "%";

    // Query exact match and prefix match with escaped LIKE
    const char* sql = R"SQL(
        SELECT e.id, e.text, ec.code
        FROM entry_codes ec
        JOIN entries e ON ec.entry_id = e.id
        WHERE ec.scheme = ? AND (ec.code = ? OR ec.code LIKE ? ESCAPE '\');
    )SQL";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) return result;
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, escapedLike);

    std::unordered_map<std::string, size_t> entryIndexMap;

    while (stmt->Step()) {
        std::string entryId = stmt->ColumnText(0);
        std::wstring text = Utf8ToWide(stmt->ColumnText(1));
        std::string matchedCode = stmt->ColumnText(2);

        MatchQuality quality = (matchedCode == lowerCode) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;

        auto it = entryIndexMap.find(entryId);
        if (it != entryIndexMap.end()) {
            Candidate& existing = result[it->second];
            if (static_cast<int>(quality) < static_cast<int>(existing.quality) ||
                (quality == existing.quality && matchedCode.length() < existing.matched_code.length())) {
                existing.quality = quality;
                existing.matched_code = matchedCode;
            }
            continue; // Deduplicate by entry_id: do not add duplicate candidate or multiply stats!
        }

        Candidate c;
        c.entry_id = entryId;
        c.text = text;
        c.scheme = scheme;
        c.matched_code = matchedCode;
        c.quality = quality;
        c.source = EntrySource::User;
        c.base_weight = 50.0; // standard user baseline weight

        // Fill preference & stats
        auto pref = GetSortPreference(scheme, lowerCode, c.entry_id);
        if (pref) {
            c.is_pinned = pref->is_pinned;
            c.order_rank = pref->order_rank;
        }

        auto stat = GetStat(scheme, lowerCode, c.entry_id);
        if (stat) {
            c.selected_count = stat->selected_count;
            c.last_selected_at = stat->last_selected_at;
            c.effective_count = CalculateDecayedCount(stat->effective_count, stat->last_decay_at, m_clock->Now());
        }

        entryIndexMap[entryId] = result.size();
        result.push_back(std::move(c));
    }
    return result;
}

bool EntryRepository::VerifyCandidateAffiliation(InputScheme scheme, const std::string& code, const std::string& entryId) {
    if (code.empty() || entryId.empty() || scheme == InputScheme::Unknown) return false;

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    // Builtin entry ids start with "b_"
    if (entryId.rfind("b_", 0) == 0) {
        return true;
    }

    std::string escapedLike = EscapeLike(lowerCode) + "%";
    const char* sql = R"SQL(
        SELECT 1 FROM entry_codes ec
        JOIN entries e ON ec.entry_id = e.id
        WHERE e.id = ? AND ec.scheme = ? AND (ec.code = ? OR ec.code LIKE ? ESCAPE '\')
        LIMIT 1;
    )SQL";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) return false;
    stmt->BindText(1, entryId);
    stmt->BindInt(2, static_cast<int>(scheme));
    stmt->BindText(3, lowerCode);
    stmt->BindText(4, escapedLike);

    return stmt->Step();
}

bool EntryRepository::SetPinPreference(InputScheme scheme, const std::string& code, const std::string& entryId, bool isPinned, int orderRank) {
    if (code.empty() || entryId.empty()) return false;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    int64_t now = m_clock->Now();
    const char* sql = R"SQL(
        INSERT INTO sort_preferences (scheme, code, entry_id, is_pinned, order_rank, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(scheme, code, entry_id) DO UPDATE SET
            is_pinned = excluded.is_pinned,
            order_rank = excluded.order_rank,
            updated_at = excluded.updated_at;
    )SQL";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) return false;
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    stmt->BindInt(4, isPinned ? 1 : 0);
    stmt->BindInt(5, orderRank);
    stmt->BindInt64(6, now);
    return stmt->Execute();
}

bool EntryRepository::UnpinPreference(InputScheme scheme, const std::string& code, const std::string& entryId) {
    return SetPinPreference(scheme, code, entryId, false, 0);
}

std::optional<SortPreference> EntryRepository::GetSortPreference(InputScheme scheme, const std::string& code, const std::string& entryId) {
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT is_pinned, order_rank FROM sort_preferences WHERE scheme = ? AND code = ? AND entry_id = ?;");
    if (!stmt) return std::nullopt;
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    if (!stmt->Step()) return std::nullopt;

    SortPreference p;
    p.scheme = scheme;
    p.code = lowerCode;
    p.entry_id = entryId;
    p.is_pinned = (stmt->ColumnInt(0) == 1);
    p.order_rank = stmt->ColumnInt(1);
    return p;
}

std::vector<SortPreference> EntryRepository::GetAllSortPreferences() {
    std::vector<SortPreference> list;
    const char* sql = "SELECT scheme, code, entry_id, is_pinned, order_rank FROM sort_preferences WHERE is_pinned = 1;";
    auto stmt = m_db.Prepare(sql);
    if (!stmt) return list;
    while (stmt->Step()) {
        SortPreference p;
        p.scheme = static_cast<InputScheme>(stmt->ColumnInt(0));
        p.code = stmt->ColumnText(1);
        p.entry_id = stmt->ColumnText(2);
        p.is_pinned = (stmt->ColumnInt(3) == 1);
        p.order_rank = stmt->ColumnInt(4);
        list.push_back(std::move(p));
    }
    return list;
}

bool EntryRepository::RecordCommit(const std::string& eventId, InputScheme scheme, const std::string& code, const std::string& entryId) {
    if (code.empty() || entryId.empty()) return false;
    int64_t now = m_clock->Now();

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    // Verify candidate affiliation
    if (!VerifyCandidateAffiliation(scheme, lowerCode, entryId)) {
        return false;
    }

    SqliteDb::Transaction tx(m_db);

    // 1. Idempotency Check with exact payload verification
    if (!eventId.empty()) {
        auto stmtEventCheck = m_db.Prepare("SELECT scheme, code, entry_id FROM commit_events WHERE event_id = ?;");
        if (stmtEventCheck) {
            stmtEventCheck->BindText(1, eventId);
            if (stmtEventCheck->Step()) {
                int prevScheme = stmtEventCheck->ColumnInt(0);
                std::string prevCode = stmtEventCheck->ColumnText(1);
                std::string prevEntryId = stmtEventCheck->ColumnText(2);
                if (prevScheme == static_cast<int>(scheme) && prevCode == lowerCode && prevEntryId == entryId) {
                    return true; // idempotent retry of identical commit
                }
                return false; // event_id reused with conflicting payload
            }
        }
    }

    // 2. Fetch existing stats for this exact (scheme, code, entry_id)
    auto statOpt = GetStat(scheme, lowerCode, entryId);
    int newCount = 1;
    double newEff = 1.0;

    if (statOpt) {
        newCount = statOpt->selected_count + 1;
        double decayed = CalculateDecayedCount(statOpt->effective_count, statOpt->last_decay_at, now);
        newEff = decayed + 1.0;
    }

    const char* sqlUpsert = R"SQL(
        INSERT INTO entry_stats (scheme, code, entry_id, selected_count, last_selected_at, effective_count, last_decay_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(scheme, code, entry_id) DO UPDATE SET
            selected_count = excluded.selected_count,
            last_selected_at = excluded.last_selected_at,
            effective_count = excluded.effective_count,
            last_decay_at = excluded.last_decay_at;
    )SQL";

    auto stmtStat = m_db.Prepare(sqlUpsert);
    if (!stmtStat) {
        tx.Rollback();
        return false;
    }
    stmtStat->BindInt(1, static_cast<int>(scheme));
    stmtStat->BindText(2, lowerCode);
    stmtStat->BindText(3, entryId);
    stmtStat->BindInt(4, newCount);
    stmtStat->BindInt64(5, now);
    stmtStat->BindDouble(6, newEff);
    stmtStat->BindInt64(7, now);
    if (!stmtStat->Execute()) {
        tx.Rollback();
        return false;
    }

    if (!eventId.empty()) {
        auto stmtEventInsert = m_db.Prepare("INSERT INTO commit_events (event_id, scheme, code, entry_id, committed_at) VALUES (?, ?, ?, ?, ?);");
        if (!stmtEventInsert) {
            tx.Rollback();
            return false;
        }
        stmtEventInsert->BindText(1, eventId);
        stmtEventInsert->BindInt(2, static_cast<int>(scheme));
        stmtEventInsert->BindText(3, lowerCode);
        stmtEventInsert->BindText(4, entryId);
        stmtEventInsert->BindInt64(5, now);
        if (!stmtEventInsert->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    return tx.Commit();
}

bool EntryRepository::SetStatDirect(InputScheme scheme, const std::string& code, const std::string& entryId,
                                   int count, int64_t lastUsed, double effCount, int64_t lastDecayAt, bool mergeMode) {
    if (code.empty() || entryId.empty()) return false;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    SqliteDb::Transaction tx(m_db);

    int finalCount = count;
    int64_t finalLastUsed = lastUsed;
    double finalEff = effCount;
    int64_t finalDecayAt = lastDecayAt;

    if (mergeMode) {
        auto existing = GetStat(scheme, lowerCode, entryId);
        if (existing) {
            finalCount = (std::max)(existing->selected_count, count);
            finalLastUsed = (std::max)(existing->last_selected_at, lastUsed);
            finalEff = (std::max)(existing->effective_count, effCount);
            finalDecayAt = (std::max)(existing->last_decay_at, lastDecayAt);
        }
    }

    const char* sqlUpsert = R"SQL(
        INSERT INTO entry_stats (scheme, code, entry_id, selected_count, last_selected_at, effective_count, last_decay_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(scheme, code, entry_id) DO UPDATE SET
            selected_count = excluded.selected_count,
            last_selected_at = excluded.last_selected_at,
            effective_count = excluded.effective_count,
            last_decay_at = excluded.last_decay_at;
    )SQL";

    auto stmt = m_db.Prepare(sqlUpsert);
    if (!stmt) return false;
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    stmt->BindInt(4, finalCount);
    stmt->BindInt64(5, finalLastUsed);
    stmt->BindDouble(6, finalEff);
    stmt->BindInt64(7, finalDecayAt);

    if (!stmt->Execute()) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

std::vector<EntryStat> EntryRepository::GetAllEntryStats() {
    std::vector<EntryStat> list;
    const char* sql = "SELECT scheme, code, entry_id, selected_count, last_selected_at, effective_count, last_decay_at FROM entry_stats WHERE selected_count > 0;";
    auto stmt = m_db.Prepare(sql);
    if (!stmt) return list;
    while (stmt->Step()) {
        EntryStat s;
        s.scheme = static_cast<InputScheme>(stmt->ColumnInt(0));
        s.code = stmt->ColumnText(1);
        s.entry_id = stmt->ColumnText(2);
        s.selected_count = stmt->ColumnInt(3);
        s.last_selected_at = stmt->ColumnInt64(4);
        s.effective_count = stmt->ColumnDouble(5);
        s.last_decay_at = stmt->ColumnInt64(6);
        list.push_back(std::move(s));
    }
    return list;
}

std::optional<EntryStat> EntryRepository::GetStat(InputScheme scheme, const std::string& code, const std::string& entryId) {
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT selected_count, last_selected_at, effective_count, last_decay_at FROM entry_stats WHERE scheme = ? AND code = ? AND entry_id = ?;");
    if (!stmt) return std::nullopt;
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    if (!stmt->Step()) return std::nullopt;

    EntryStat s;
    s.selected_count = stmt->ColumnInt(0);
    s.last_selected_at = stmt->ColumnInt64(1);
    s.effective_count = stmt->ColumnDouble(2);
    s.last_decay_at = stmt->ColumnInt64(3);
    return s;
}

bool EntryRepository::ResetStats(const std::string& entryId) {
    if (entryId.empty()) {
        return m_db.Execute("DELETE FROM entry_stats; DELETE FROM commit_events;");
    } else {
        auto stmt = m_db.Prepare("DELETE FROM entry_stats WHERE entry_id = ?;");
        if (!stmt) return false;
        stmt->BindText(1, entryId);
        stmt->Step();
        return true;
    }
}

void EntryRepository::EnsureBuiltinEntryExists(const std::string& entryId, const std::wstring& text) {
    auto stmt = m_db.Prepare("INSERT OR IGNORE INTO entries (id, text, source, created_at, updated_at) VALUES (?, ?, 0, ?, ?);");
    if (stmt) {
        int64_t now = m_clock->Now();
        stmt->BindText(1, entryId);
        stmt->BindText(2, WideToUtf8(text));
        stmt->BindInt64(3, now);
        stmt->BindInt64(4, now);
        stmt->Step();
    }
}

} // namespace ime

