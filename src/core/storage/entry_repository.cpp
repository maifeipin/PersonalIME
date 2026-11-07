#include "entry_repository.h"
#include "../dictionary/builtin_dict.h"
#include "../encoding/shuangpin_decoder.h"
#include <cmath>
#include <sstream>
#include <random>
#include <iomanip>
#include <unordered_map>
#include <set>

namespace ime {

static constexpr double HALF_LIFE_SECONDS = 30.0 * 86400.0; // 30 days

static std::string BuildSentencePayloadFingerprint(uint64_t sessionId, uint64_t contextId,
                                                   InputScheme scheme, const std::string& scopedLearningCode,
                                                   const std::wstring& text,
                                                   const std::string& storagePinyinCode,
                                                   const std::string& originalCandidateId) {
    std::string lowerScoped = scopedLearningCode;
    std::transform(lowerScoped.begin(), lowerScoped.end(), lowerScoped.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string lowerStorage = storagePinyinCode;
    std::transform(lowerStorage.begin(), lowerStorage.end(), lowerStorage.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string layoutId;
    std::string rawQuery;
    if (scheme == InputScheme::Shuangpin) {
        auto colonPos = lowerScoped.find(':');
        if (colonPos != std::string::npos) {
            layoutId = lowerScoped.substr(0, colonPos);
            rawQuery = lowerScoped.substr(colonPos + 1);
        } else {
            rawQuery = lowerScoped;
        }
    } else {
        rawQuery = lowerScoped;
    }

    std::ostringstream oss;
    oss << "s=" << sessionId << ";"
        << "c=" << contextId << ";"
        << "sch=" << static_cast<int>(scheme) << ";"
        << "lay=" << layoutId << ";"
        << "raw=" << rawQuery << ";"
        << "sc=" << lowerScoped << ";"
        << "txt=" << WideToUtf8(text) << ";"
        << "stg=" << lowerStorage << ";"
        << "cid=" << originalCandidateId;
    return oss.str();
}

static std::string BuildCommitPayloadFingerprint(uint64_t sessionId, uint64_t contextId,
                                                InputScheme scheme, const std::string& code,
                                                const std::string& entryId) {
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
    std::ostringstream oss;
    oss << "s=" << sessionId << ";c=" << contextId << ";sch=" << static_cast<int>(scheme) << ";sc=" << lowerCode << ";eid=" << entryId;
    return oss.str();
}

// Validate the whole replacement before deleting any existing mappings.
static bool ValidateCodes(const std::vector<std::pair<InputScheme, std::string>>& codes) {
    if (codes.empty()) return false;
    for (const auto& [scheme, code] : codes) {
        if (!IsValidCode(scheme, code)) return false;
    }
    return true;
}

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
    if (!IsValidText(text) || !ValidateCodes(codes)) return "";

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
    if (!tx.IsStarted()) return "";

    auto stmtEntry = m_db.Prepare("INSERT INTO entries (id, text, source, created_at, updated_at) VALUES (?, ?, ?, ?, ?);");
    if (!stmtEntry) return "";
    stmtEntry->BindText(1, id);
    stmtEntry->BindText(2, textUtf8);
    stmtEntry->BindInt(3, static_cast<int>(source));
    stmtEntry->BindInt64(4, now);
    stmtEntry->BindInt64(5, now);
    if (!stmtEntry->Execute()) {
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
        if (!stmtCode->Execute()) {
            tx.Rollback();
            return "";
        }
    }

    if (!tx.Commit()) {
        return "";
    }
    return id;
}

bool EntryRepository::AddEntryCodes(const std::string& entryId, const std::vector<std::pair<InputScheme, std::string>>& codes) {
    if (entryId.empty() || !ValidateCodes(codes)) return false;
    int64_t now = m_clock->Now();
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

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
        if (!stmtCode->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    return tx.Commit();
}

bool EntryRepository::SetEntryCodes(const std::string& entryId, const std::vector<std::pair<InputScheme, std::string>>& codes) {
    if (entryId.empty() || !ValidateCodes(codes)) return false;
    int64_t now = m_clock->Now();
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;
    if (HasPendingCommitsForEntry(entryId)) {
        tx.Rollback();
        return false;
    }
    if (!GetEntry(entryId)) {
        tx.Rollback();
        return false;
    }

    auto stmtDel = m_db.Prepare("DELETE FROM entry_codes WHERE entry_id = ?;");
    if (!stmtDel) {
        tx.Rollback();
        return false;
    }
    stmtDel->BindText(1, entryId);
    if (!stmtDel->Execute()) {
        tx.Rollback();
        return false;
    }

    auto stmtCode = m_db.Prepare("INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at) VALUES (?, ?, ?, ?);");
    if (!stmtCode) {
        tx.Rollback();
        return false;
    }

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
        if (!stmtCode->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    return tx.Commit();
}

bool EntryRepository::UpdateEntry(const std::string& entryId, const std::wstring& newText, const std::vector<std::pair<InputScheme, std::string>>& newCodes) {
    if (entryId.empty() || !IsValidText(newText) ||
        (!newCodes.empty() && !ValidateCodes(newCodes))) return false;
    int64_t now = m_clock->Now();
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;
    if (HasPendingCommitsForEntry(entryId)) {
        tx.Rollback();
        return false;
    }

    auto stmt = m_db.Prepare("UPDATE entries SET text = ?, updated_at = ? WHERE id = ?;");
    if (!stmt) {
        tx.Rollback();
        return false;
    }
    stmt->BindText(1, WideToUtf8(newText));
    stmt->BindInt64(2, now);
    stmt->BindText(3, entryId);
    if (!stmt->Execute() || m_db.Changes() == 0) {
        tx.Rollback();
        return false;
    }

    if (!newCodes.empty()) {
        auto stmtDel = m_db.Prepare("DELETE FROM entry_codes WHERE entry_id = ?;");
        if (!stmtDel) {
            tx.Rollback();
            return false;
        }
        stmtDel->BindText(1, entryId);
        if (!stmtDel->Execute()) {
            tx.Rollback();
            return false;
        }

        auto stmtCode = m_db.Prepare("INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at) VALUES (?, ?, ?, ?);");
        if (!stmtCode) {
            tx.Rollback();
            return false;
        }

        for (const auto& [scheme, code] : newCodes) {
            if (scheme == InputScheme::Unknown) continue;
            std::string lowerCode = code;
            std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
            if (!IsValidCode(scheme, lowerCode)) continue;

            stmtCode->Reset();
            stmtCode->BindText(1, entryId);
            stmtCode->BindInt(2, static_cast<int>(scheme));
            stmtCode->BindText(3, lowerCode);
            stmtCode->BindInt64(4, now);
            if (!stmtCode->Execute()) {
                tx.Rollback();
                return false;
            }
        }
    }

    return tx.Commit();
}

bool EntryRepository::UpdateEntryText(const std::string& entryId, const std::wstring& newText) {
    if (entryId.empty() || !IsValidText(newText)) return false;
    int64_t now = m_clock->Now();
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;
    if (HasPendingCommitsForEntry(entryId)) {
        tx.Rollback();
        return false;
    }
    std::string textUtf8 = WideToUtf8(newText);

    auto stmt = m_db.Prepare("UPDATE entries SET text = ?, updated_at = ? WHERE id = ?;");
    if (!stmt) {
        tx.Rollback();
        return false;
    }
    stmt->BindText(1, textUtf8);
    stmt->BindInt64(2, now);
    stmt->BindText(3, entryId);
    if (!stmt->Execute() || m_db.Changes() == 0) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

bool EntryRepository::DeleteEntry(const std::string& entryId) {
    if (entryId.empty()) return false;
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;
    if (HasPendingCommitsForEntry(entryId)) {
        tx.Rollback();
        return false;
    }

    auto stmt = m_db.Prepare("DELETE FROM entries WHERE id = ?;");
    if (!stmt) return false;
    stmt->BindText(1, entryId);
    if (!stmt->Execute() || m_db.Changes() == 0) {
        tx.Rollback();
        return false;
    }

    // Cascade manually if foreign keys are disabled
    auto s1 = m_db.Prepare("DELETE FROM entry_codes WHERE entry_id = ?;");
    if (s1) { s1->BindText(1, entryId); if (!s1->Execute()) { tx.Rollback(); return false; } }
    auto s2 = m_db.Prepare("DELETE FROM sort_preferences WHERE entry_id = ?;");
    if (s2) { s2->BindText(1, entryId); if (!s2->Execute()) { tx.Rollback(); return false; } }
    auto s3 = m_db.Prepare("DELETE FROM entry_stats WHERE entry_id = ?;");
    if (s3) { s3->BindText(1, entryId); if (!s3->Execute()) { tx.Rollback(); return false; } }

    return tx.Commit();
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
    if (!stmt) throw std::runtime_error("Cannot read backup entries");

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
    if (stmt->HasError() || !stmtCodes || !stmtMetaCodes) throw std::runtime_error("Cannot read backup codes");

    for (auto& e : list) {
        if (stmtCodes) {
            stmtCodes->Reset();
            stmtCodes->BindText(1, e.id);
            while (stmtCodes->Step()) {
                InputScheme sch = static_cast<InputScheme>(stmtCodes->ColumnInt(0));
                std::string code = stmtCodes->ColumnText(1);
                e.codes.emplace_back(sch, code);
            }
            if (stmtCodes->HasError()) throw std::runtime_error("Cannot read entry codes");
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
            if (stmtMetaCodes->HasError()) throw std::runtime_error("Cannot read builtin metadata codes");
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
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("QueryUserCandidates prepare failed: " + m_db.GetLastError());
        }
        return result;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, escapedLike);

    std::unordered_map<std::string, size_t> entryCodeMap;

    while (stmt->Step()) {
        std::string entryId = stmt->ColumnText(0);
        std::wstring text = Utf8ToWide(stmt->ColumnText(1));
        std::string matchedCode = stmt->ColumnText(2);

        MatchQuality quality = (matchedCode == lowerCode) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;

        std::string mappingKey = entryId + ":" + matchedCode;
        auto it = entryCodeMap.find(mappingKey);
        if (it != entryCodeMap.end()) {
            Candidate& existing = result[it->second];
            if (static_cast<int>(quality) < static_cast<int>(existing.quality)) {
                existing.quality = quality;
            }
            continue;
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

        entryCodeMap[mappingKey] = result.size();
        result.push_back(std::move(c));
    }
    if (stmt->HasError()) {
        throw std::runtime_error("QueryUserCandidates step failed: " + stmt->LastErrorMessage());
    }
    return result;
}

std::vector<std::string> EntryRepository::GetUserCodesForText(const std::wstring& text, InputScheme scheme) const {
    std::vector<std::string> result;
    if (text.empty()) return result;

    std::string textUtf8 = WideToUtf8(text);
    if (textUtf8.empty()) return result;

    std::string sql;
    if (scheme == InputScheme::Unknown) {
        sql = "SELECT ec.code FROM entry_codes ec JOIN entries e ON ec.entry_id = e.id WHERE e.text = ? ORDER BY ec.id ASC;";
    } else {
        sql = "SELECT ec.code FROM entry_codes ec JOIN entries e ON ec.entry_id = e.id WHERE e.text = ? AND ec.scheme = ? ORDER BY ec.id ASC;";
    }

    auto stmt = m_db.Prepare(sql);
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("GetUserCodesForText prepare failed: " + m_db.GetLastError());
        }
        return result;
    }
    stmt->BindText(1, textUtf8);
    if (scheme != InputScheme::Unknown) {
        stmt->BindInt(2, static_cast<int>(scheme));
    }

    std::unordered_set<std::string> seen;
    while (stmt->Step()) {
        std::string code = stmt->ColumnText(0);
        if (!code.empty() && seen.insert(code).second) {
            result.push_back(std::move(code));
        }
    }
    if (stmt->LastErrorCode() != SQLITE_DONE && stmt->LastErrorCode() != SQLITE_OK) {
        throw std::runtime_error("GetUserCodesForText step failed: " + stmt->LastErrorMessage());
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

    // Strip layout prefix if scoped code was passed (e.g. "natural:zhongwen" -> "zhongwen")
    std::string cleanCode = lowerCode;
    auto colonPos = cleanCode.find(':');
    if (colonPos != std::string::npos) {
        cleanCode = cleanCode.substr(colonPos + 1);
    }

    std::string cleanNoApos = cleanCode;
    cleanNoApos.erase(std::remove(cleanNoApos.begin(), cleanNoApos.end(), '\''), cleanNoApos.end());

    const char* sql = R"SQL(
        SELECT ec.code FROM entry_codes ec
        JOIN entries e ON ec.entry_id = e.id
        WHERE e.id = ?
          AND (ec.scheme = ? OR (? = 4 AND ec.scheme = 1));
    )SQL";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("VerifyCandidateAffiliation prepare failed: " + m_db.GetLastError());
        }
        return false;
    }
    stmt->BindText(1, entryId);
    stmt->BindInt(2, static_cast<int>(scheme));
    stmt->BindInt(3, static_cast<int>(scheme)); // 4 is Shuangpin, 1 is Pinyin

    while (stmt->Step()) {
        std::string rCode = stmt->ColumnText(0);
        std::transform(rCode.begin(), rCode.end(), rCode.begin(), [](unsigned char c) { return std::tolower(c); });

        if (rCode == cleanCode || rCode.rfind(cleanCode, 0) == 0) {
            return true;
        }

        std::string rNoApos = rCode;
        rNoApos.erase(std::remove(rNoApos.begin(), rNoApos.end(), '\''), rNoApos.end());
        if (rNoApos == cleanNoApos || rNoApos.rfind(cleanNoApos, 0) == 0) {
            return true;
        }
    }

    if (stmt->HasError()) {
        throw std::runtime_error("VerifyCandidateAffiliation step failed: " + stmt->LastErrorMessage());
    }
    return false;
}

bool EntryRepository::HasCommitEvent(const std::string& eventId) {
    if (eventId.empty()) return false;
    auto stmt = m_db.Prepare("SELECT 1 FROM commit_events WHERE event_id = ?;");
    if (!stmt) return false;
    stmt->BindText(1, eventId);
    return stmt->Step();
}

EntryRepository::CommitEventRecord EntryRepository::GetCommitEventRecord(const std::string& eventId) {
    CommitEventRecord rec;
    if (eventId.empty()) return rec;
    auto stmt = m_db.Prepare("SELECT session_id, context_id, status, committed_at FROM commit_events WHERE event_id = ?;");
    if (!stmt) return rec;
    stmt->BindText(1, eventId);
    if (stmt->Step()) {
        rec.event_id = eventId;
        rec.session_id = static_cast<uint64_t>(stmt->ColumnInt64(0));
        rec.context_id = static_cast<uint64_t>(stmt->ColumnInt64(1));
        rec.status = stmt->ColumnInt(2);
        rec.committed_at = stmt->ColumnInt64(3);
    }
    return rec;
}

bool EntryRepository::SavePendingCommit(const PendingCommitRecord& rec) {
    if (rec.event_id.empty()) return false;
    // Use INSERT (not REPLACE) so existing event IDs are never overwritten
    auto stmt = m_db.Prepare(
        "INSERT INTO pending_commits "
        "(event_id, session_id, context_id, scheme, query_code, entry_id, text, created_at, "
        "learning_enabled, layout_id, matched_code, resolved_code, global_epoch, entry_epoch) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
    );
    if (!stmt) return false;
    stmt->BindText(1, rec.event_id);
    stmt->BindInt64(2, static_cast<int64_t>(rec.session_id));
    stmt->BindInt64(3, static_cast<int64_t>(rec.context_id));
    stmt->BindInt(4, static_cast<int>(rec.scheme));
    stmt->BindText(5, rec.query_code);
    stmt->BindText(6, rec.entry_id);
    stmt->BindText(7, WideToUtf8(rec.text));
    stmt->BindInt64(8, rec.created_at);
    stmt->BindInt(9, rec.learning_enabled ? 1 : 0);
    stmt->BindText(10, rec.layout_id);
    stmt->BindText(11, rec.matched_code);
    stmt->BindText(12, rec.resolved_code);

    int64_t gEpoch = rec.global_epoch > 0 ? rec.global_epoch : 1;
    if (rec.global_epoch <= 0) {
        auto stmtGlobal = m_db.Prepare("SELECT value FROM meta_settings WHERE key = 'global_stats_reset_epoch';");
        if (!stmtGlobal) {
            return false;
        }
        if (stmtGlobal->Step()) {
            try {
                gEpoch = std::stoll(stmtGlobal->ColumnText(0));
            } catch (...) {
                gEpoch = 1;
            }
        } else if (stmtGlobal->HasError()) {
            return false;
        }
    }

    int64_t eEpoch = rec.entry_epoch > 0 ? rec.entry_epoch : 1;
    if (rec.entry_epoch <= 0) {
        if (rec.entry_id.rfind("sentence_", 0) == 0) {
            std::string pinyinSource = (rec.scheme == InputScheme::Shuangpin && !rec.resolved_code.empty())
                ? rec.resolved_code
                : (!rec.query_code.empty() ? rec.query_code : rec.matched_code);
            std::string storageCode;
            for (char ch : pinyinSource) {
                if (ch != '\'' && ch != ' ') {
                    storageCode.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                }
            }
            std::string textUtf8 = WideToUtf8(rec.text);
            auto stmtFind = m_db.Prepare(
                "SELECT e.id FROM entries e JOIN entry_codes ec ON e.id = ec.entry_id "
                "WHERE e.text = ? AND ec.scheme = 1 AND (ec.code = ? OR ec.code = ?) LIMIT 1;"
            );
            if (!stmtFind) {
                return false;
            }
            stmtFind->BindText(1, textUtf8);
            stmtFind->BindText(2, storageCode);
            stmtFind->BindText(3, rec.query_code);
            if (stmtFind->Step()) {
                std::string existingId = stmtFind->ColumnText(0);
                auto stmtEpoch = m_db.Prepare("SELECT reset_epoch FROM entry_resets WHERE entry_id = ?;");
                if (!stmtEpoch) {
                    return false;
                }
                stmtEpoch->BindText(1, existingId);
                if (stmtEpoch->Step()) {
                    eEpoch = stmtEpoch->ColumnInt64(0);
                } else if (stmtEpoch->HasError()) {
                    return false;
                }
            } else if (stmtFind->HasError()) {
                return false;
            }
        } else {
            auto stmtEpoch = m_db.Prepare("SELECT reset_epoch FROM entry_resets WHERE entry_id = ?;");
            if (!stmtEpoch) {
                return false;
            }
            stmtEpoch->BindText(1, rec.entry_id);
            if (stmtEpoch->Step()) {
                eEpoch = stmtEpoch->ColumnInt64(0);
            } else if (stmtEpoch->HasError()) {
                return false;
            }
        }
    }
    stmt->BindInt64(13, gEpoch);
    stmt->BindInt64(14, eEpoch);
    return stmt->Execute();
}

bool EntryRepository::RemovePendingCommit(const std::string& eventId) {
    if (eventId.empty()) return false;
    auto stmt = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
    if (!stmt) return false;
    stmt->BindText(1, eventId);
    return stmt->Execute();
}

bool EntryRepository::RecordCancel(const std::string& eventId, uint64_t sessionId, uint64_t contextId) {
    if (eventId.empty()) return false;
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    // 1. Check if already recorded or cancelled in commit_events
    auto stmtCheck = m_db.Prepare("SELECT session_id, context_id, status FROM commit_events WHERE event_id = ?;");
    if (stmtCheck) {
        stmtCheck->BindText(1, eventId);
        if (stmtCheck->Step()) {
            uint64_t prevSid = static_cast<uint64_t>(stmtCheck->ColumnInt64(0));
            uint64_t prevCid = static_cast<uint64_t>(stmtCheck->ColumnInt64(1));
            int prevStatus = stmtCheck->ColumnInt(2);
            if (prevSid != sessionId || prevCid != contextId) {
                tx.Rollback();
                return false;
            }
            if (prevStatus == 1) {
                // Recorded commit cannot be cancelled!
                tx.Rollback();
                return false;
            } else if (prevStatus == 2) {
                // Already cancelled by same owner: idempotent success
                return true;
            }
        }
    }

    // 2. Delete from pending_commits and check execution
    auto stmtDel = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
    if (!stmtDel) {
        tx.Rollback();
        return false;
    }
    stmtDel->BindText(1, eventId);
    if (!stmtDel->Execute()) {
        tx.Rollback();
        return false;
    }

    // 3. Insert durable Cancelled record into commit_events
    int64_t now = m_clock ? m_clock->Now() : 0;
    auto stmtInsert = m_db.Prepare("INSERT OR REPLACE INTO commit_events (event_id, session_id, context_id, scheme, code, entry_id, committed_at, status) VALUES (?, ?, ?, 0, '', '', ?, 2);");
    if (!stmtInsert) {
        tx.Rollback();
        return false;
    }
    stmtInsert->BindText(1, eventId);
    stmtInsert->BindInt64(2, static_cast<int64_t>(sessionId));
    stmtInsert->BindInt64(3, static_cast<int64_t>(contextId));
    stmtInsert->BindInt64(4, now);
    if (!stmtInsert->Execute()) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool EntryRepository::LoadAllPendingCommits(std::vector<PendingCommitRecord>& outRecords) {
    outRecords.clear();
    auto stmt = m_db.Prepare(
        "SELECT event_id, session_id, context_id, scheme, query_code, entry_id, text, created_at, "
        "learning_enabled, layout_id, matched_code, resolved_code, global_epoch, entry_epoch "
        "FROM pending_commits;"
    );
    if (!stmt) return false;
    while (stmt->Step()) {
        PendingCommitRecord r;
        r.event_id = stmt->ColumnText(0);
        r.session_id = static_cast<uint64_t>(stmt->ColumnInt64(1));
        r.context_id = static_cast<uint64_t>(stmt->ColumnInt64(2));
        r.scheme = static_cast<InputScheme>(stmt->ColumnInt(3));
        r.query_code = stmt->ColumnText(4);
        r.entry_id = stmt->ColumnText(5);
        r.text = Utf8ToWide(stmt->ColumnText(6));
        r.created_at = stmt->ColumnInt64(7);
        r.learning_enabled = stmt->ColumnInt(8) != 0;
        r.layout_id = stmt->ColumnText(9);
        r.matched_code = stmt->ColumnText(10);
        r.resolved_code = stmt->ColumnText(11);
        r.global_epoch = stmt->ColumnInt64(12);
        r.entry_epoch = stmt->ColumnInt64(13);
        outRecords.push_back(std::move(r));
    }
    if (stmt->HasError()) {
        outRecords.clear();
        return false;
    }
    return true;
}

bool EntryRepository::SetPinPreference(InputScheme scheme, const std::string& code, const std::string& entryId, bool isPinned, int orderRank,
                                       std::optional<uint64_t> expectedRevision, uint64_t* outRevision, bool* outConflict) {
    if (outConflict) *outConflict = false;
    if (code.empty() || entryId.empty()) return false;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    // Builtin entry materialization inside transaction
    if (isPinned && entryId.rfind("b_", 0) == 0) {
        if (!m_builtinDict) {
            tx.Rollback();
            return false;
        }
        auto textOpt = m_builtinDict->GetTextById(entryId);
        if (!textOpt || textOpt->empty()) {
            tx.Rollback();
            return false;
        }
        if (!EnsureBuiltinEntryExists(entryId, *textOpt)) {
            tx.Rollback();
            return false;
        }
    }

    uint64_t curRev = GetPinnedRevision(scheme, lowerCode);
    if (expectedRevision.has_value() && *expectedRevision != curRev) {
        if (outConflict) *outConflict = true;
        tx.Rollback();
        return false;
    }

    int64_t now = m_clock ? m_clock->Now() : 0;
    const char* sql = R"SQL(
        INSERT INTO sort_preferences (scheme, code, entry_id, is_pinned, order_rank, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(scheme, code, entry_id) DO UPDATE SET
            is_pinned = excluded.is_pinned,
            order_rank = excluded.order_rank,
            updated_at = excluded.updated_at;
    )SQL";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) {
        tx.Rollback();
        return false;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    stmt->BindInt(4, isPinned ? 1 : 0);
    stmt->BindInt(5, orderRank);
    stmt->BindInt64(6, now);
    if (!stmt->Execute()) {
        tx.Rollback();
        return false;
    }

    uint64_t nextRev = curRev + 1;
    if (!SetPinnedRevision(scheme, lowerCode, nextRev)) {
        tx.Rollback();
        return false;
    }
    if (outRevision) *outRevision = nextRev;

    return tx.Commit();
}

bool EntryRepository::UnpinPreference(InputScheme scheme, const std::string& code, const std::string& entryId,
                                     std::optional<uint64_t> expectedRevision, uint64_t* outRevision, bool* outConflict) {
    return SetPinPreference(scheme, code, entryId, false, 0, expectedRevision, outRevision, outConflict);
}

std::optional<SortPreference> EntryRepository::GetSortPreference(InputScheme scheme, const std::string& code, const std::string& entryId) {
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT is_pinned, order_rank FROM sort_preferences WHERE scheme = ? AND code = ? AND entry_id = ?;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("GetSortPreference prepare failed: " + m_db.GetLastError());
        }
        return std::nullopt;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    if (!stmt->Step()) {
        if (stmt->HasError()) {
            throw std::runtime_error("GetSortPreference step failed: " + stmt->LastErrorMessage());
        }
        return std::nullopt;
    }

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
    if (!stmt) throw std::runtime_error("Cannot read backup preferences");
    while (stmt->Step()) {
        SortPreference p;
        p.scheme = static_cast<InputScheme>(stmt->ColumnInt(0));
        p.code = stmt->ColumnText(1);
        p.entry_id = stmt->ColumnText(2);
        p.is_pinned = (stmt->ColumnInt(3) == 1);
        p.order_rank = stmt->ColumnInt(4);
        list.push_back(std::move(p));
    }
    if (stmt->HasError()) throw std::runtime_error("Backup preference read failed");
    return list;
}

std::vector<SortPreference> EntryRepository::GetPinnedPreferences(InputScheme scheme, const std::string& code) {
    std::vector<SortPreference> list;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT scheme, code, entry_id, is_pinned, order_rank FROM sort_preferences WHERE scheme = ? AND code = ? AND is_pinned = 1 ORDER BY order_rank ASC;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("GetPinnedPreferences prepare failed: " + m_db.GetLastError());
        }
        return list;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    while (stmt->Step()) {
        SortPreference p;
        p.scheme = scheme;
        p.code = lowerCode;
        p.entry_id = stmt->ColumnText(2);
        p.is_pinned = (stmt->ColumnInt(3) == 1);
        p.order_rank = stmt->ColumnInt(4);
        list.push_back(std::move(p));
    }
    if (stmt->HasError()) {
        throw std::runtime_error("GetPinnedPreferences step failed: " + stmt->LastErrorMessage());
    }
    return list;
}

bool EntryRepository::RecordCommit(const std::string& eventId, uint64_t sessionId, uint64_t contextId,
                                   InputScheme scheme, const std::string& code, const std::string& entryId,
                                   bool updateStats,
                                   InputScheme affiliationScheme,
                                   const std::string& affiliationCode) {
    if (code.empty() || entryId.empty()) return false;
    int64_t now = m_clock->Now();

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    InputScheme affScheme = (affiliationScheme != InputScheme::Unknown) ? affiliationScheme : scheme;
    std::string affCode = !affiliationCode.empty() ? affiliationCode : lowerCode;

    // Verify candidate affiliation
    if (!VerifyCandidateAffiliation(affScheme, affCode, entryId)) {
        // Verified historical snapshot fallback (P1/Requirement 7):
        // If entry was deleted or codes modified while commit was in-flight,
        // verify against the verified pending_commits snapshot so WAL receipt is never lost!
        bool hasVerifiedSnapshot = false;
        if (!eventId.empty()) {
            auto stmtPending = m_db.Prepare("SELECT entry_id FROM pending_commits WHERE event_id = ?;");
            if (stmtPending) {
                stmtPending->BindText(1, eventId);
                if (stmtPending->Step() && stmtPending->ColumnText(0) == entryId) {
                    hasVerifiedSnapshot = true;
                }
            }
        }
        if (!hasVerifiedSnapshot) {
            return false;
        }
    }

    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    // 1. Idempotency Check with exact payload and owner verification
    if (!eventId.empty()) {
        auto stmtEventCheck = m_db.Prepare("SELECT session_id, context_id, scheme, code, entry_id, status, payload_hash FROM commit_events WHERE event_id = ?;");
        if (stmtEventCheck) {
            stmtEventCheck->BindText(1, eventId);
            if (stmtEventCheck->Step()) {
                uint64_t prevSid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(0));
                uint64_t prevCid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(1));
                int prevScheme = stmtEventCheck->ColumnInt(2);
                std::string prevCode = stmtEventCheck->ColumnText(3);
                std::string prevEntryId = stmtEventCheck->ColumnText(4);
                int prevStatus = stmtEventCheck->ColumnInt(5);
                std::string prevPayload = stmtEventCheck->ColumnText(6);

                if (prevStatus == 2) {
                    // Cancelled commit can NEVER become Recorded!
                    tx.Rollback();
                    return false;
                }

                // If session_id and context_id were stored (non-zero), verify owner
                if (prevSid != 0 || prevCid != 0) {
                    if (sessionId != 0 && contextId != 0 && (prevSid != sessionId || prevCid != contextId)) {
                        tx.Rollback();
                        return false; // Wrong owner replaying confirmation
                    }
                }

                if (!prevPayload.empty()) {
                    std::string expectedFp = BuildCommitPayloadFingerprint(sessionId, contextId, scheme, lowerCode, entryId);
                    if (prevPayload == expectedFp) {
                        return true;
                    }
                    tx.Rollback();
                    return false;
                }

                if (prevEntryId.empty() ||
                    (prevScheme == static_cast<int>(scheme) && prevCode == lowerCode && prevEntryId == entryId)) {
                    return true; // idempotent retry of identical commit (or migrated v1 event)
                }
                tx.Rollback();
                return false; // event_id reused with conflicting payload
            }
        }
    }

    // 2. Fetch existing stats for this exact (scheme, code, entry_id) and update if updateStats is true
    if (updateStats) {
        if (!eventId.empty()) {
            auto stmtPending = m_db.Prepare("SELECT global_epoch, entry_epoch, created_at FROM pending_commits WHERE event_id = ?;");
            if (stmtPending) {
                stmtPending->BindText(1, eventId);
                if (stmtPending->Step()) {
                    int64_t pGlobalEpoch = stmtPending->ColumnInt64(0);
                    int64_t pEntryEpoch = stmtPending->ColumnInt64(1);
                    int64_t pCreatedAt = stmtPending->ColumnInt64(2);
                    int64_t curGlobalEpoch = GetGlobalStatsResetEpoch();
                    int64_t curEntryEpoch = GetEntryStatsResetEpoch(entryId);
                    if (pGlobalEpoch < curGlobalEpoch || pEntryEpoch < curEntryEpoch) {
                        updateStats = false; // Reset occurred after pending commit was created!
                    }
                    if (pGlobalEpoch == 0 && pEntryEpoch == 0) {
                        // Legacy v3 records without monotonic epoch: conservatively compare against wall-clock
                        int64_t lastReset = GetLastResetStatsAt();
                        if (lastReset > 0 && pCreatedAt > 0 && pCreatedAt < lastReset) {
                            updateStats = false;
                        }
                    }
                }
            }
        }
    }

    if (updateStats) {
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
    }

    if (!eventId.empty()) {
        // Delete pending commit and check execution!
        auto stmtDelPending = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
        if (stmtDelPending) {
            stmtDelPending->BindText(1, eventId);
            if (!stmtDelPending->Execute()) {
                tx.Rollback();
                return false;
            }
        }

        auto stmtEventInsert = m_db.Prepare("INSERT OR REPLACE INTO commit_events (event_id, session_id, context_id, scheme, code, entry_id, committed_at, status, payload_hash) VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?);");
        if (!stmtEventInsert) {
            tx.Rollback();
            return false;
        }
        std::string fp = BuildCommitPayloadFingerprint(sessionId, contextId, scheme, lowerCode, entryId);
        stmtEventInsert->BindText(1, eventId);
        stmtEventInsert->BindInt64(2, static_cast<int64_t>(sessionId));
        stmtEventInsert->BindInt64(3, static_cast<int64_t>(contextId));
        stmtEventInsert->BindInt(4, static_cast<int>(scheme));
        stmtEventInsert->BindText(5, lowerCode);
        stmtEventInsert->BindText(6, entryId);
        stmtEventInsert->BindInt64(7, now);
        stmtEventInsert->BindText(8, fp);
        if (!stmtEventInsert->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    return tx.Commit();
}

bool EntryRepository::RecordTransientCommitEvent(const std::string& eventId, uint64_t sessionId, uint64_t contextId,
                                                 InputScheme scheme, const std::string& code, const std::string& entryId) {
    if (eventId.empty()) return false;
    int64_t now = m_clock ? m_clock->Now() : 0;

    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    std::string currentFp = BuildCommitPayloadFingerprint(sessionId, contextId, scheme, lowerCode, entryId);

    // 1. Existing commit_events check (immutable terminal receipt contract)
    auto stmtEventCheck = m_db.Prepare("SELECT session_id, context_id, scheme, code, entry_id, status, payload_hash FROM commit_events WHERE event_id = ?;");
    if (!stmtEventCheck) {
        tx.Rollback();
        return false;
    }
    stmtEventCheck->BindText(1, eventId);
    if (stmtEventCheck->Step()) {
        uint64_t prevSid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(0));
        uint64_t prevCid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(1));
        int prevScheme = stmtEventCheck->ColumnInt(2);
        std::string prevCode = stmtEventCheck->ColumnText(3);
        std::string prevEntryId = stmtEventCheck->ColumnText(4);
        int prevStatus = stmtEventCheck->ColumnInt(5);
        std::string prevPayload = stmtEventCheck->ColumnText(6);

        if (prevStatus == 2) {
            tx.Rollback();
            return false; // Cancelled event can NEVER become Recorded!
        }
        if (sessionId != prevSid || contextId != prevCid) {
            tx.Rollback();
            return false; // Owner mismatch
        }
        if (!prevPayload.empty()) {
            if (prevPayload == currentFp) {
                return true;
            }
            tx.Rollback();
            return false;
        }
        if (prevScheme == static_cast<int>(scheme) && prevCode == lowerCode && prevEntryId == entryId) {
            return true; // Idempotent replay of identical commit
        }
        tx.Rollback();
        return false; // Conflict payload replay rejected
    }
    if (stmtEventCheck->HasError()) {
        tx.Rollback();
        return false;
    }

    // 2. Strict pending_commits snapshot verification (Normal ACK must have trusted pending)
    auto stmtPending = m_db.Prepare("SELECT session_id, context_id, scheme, query_code, entry_id FROM pending_commits WHERE event_id = ?;");
    if (!stmtPending) {
        tx.Rollback();
        return false;
    }
    stmtPending->BindText(1, eventId);
    if (!stmtPending->Step()) {
        if (stmtPending->HasError()) {
            tx.Rollback();
            return false;
        }
        // Unknown event has neither existing commit_event nor pending_commit: fail-closed!
        tx.Rollback();
        return false;
    }

    uint64_t pendSid = static_cast<uint64_t>(stmtPending->ColumnInt64(0));
    uint64_t pendCid = static_cast<uint64_t>(stmtPending->ColumnInt64(1));
    int pendScheme = stmtPending->ColumnInt(2);
    std::string pendCode = stmtPending->ColumnText(3);
    std::string pendEntryId = stmtPending->ColumnText(4);
    std::transform(pendCode.begin(), pendCode.end(), pendCode.begin(), [](unsigned char c) { return std::tolower(c); });

    if (pendSid != sessionId || pendCid != contextId) {
        tx.Rollback();
        return false; // Ownership mismatch against pending
    }
    if (pendScheme != static_cast<int>(scheme) || pendCode != lowerCode || pendEntryId != entryId) {
        tx.Rollback();
        return false; // Payload tampering rejected
    }

    // 3. Delete pending commit and verify execution
    auto stmtDelPending = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
    if (!stmtDelPending) {
        tx.Rollback();
        return false;
    }
    stmtDelPending->BindText(1, eventId);
    if (!stmtDelPending->Execute()) {
        tx.Rollback();
        return false;
    }

    // 4. Insert new commit event using INSERT (NOT REPLACE) to prevent overwriting
    auto stmtEventInsert = m_db.Prepare("INSERT INTO commit_events (event_id, session_id, context_id, scheme, code, entry_id, committed_at, status, payload_hash) VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?);");
    if (!stmtEventInsert) {
        tx.Rollback();
        return false;
    }
    stmtEventInsert->BindText(1, eventId);
    stmtEventInsert->BindInt64(2, static_cast<int64_t>(sessionId));
    stmtEventInsert->BindInt64(3, static_cast<int64_t>(contextId));
    stmtEventInsert->BindInt(4, static_cast<int>(scheme));
    stmtEventInsert->BindText(5, lowerCode);
    stmtEventInsert->BindText(6, entryId);
    stmtEventInsert->BindInt64(7, now);
    stmtEventInsert->BindText(8, currentFp);
    if (!stmtEventInsert->Execute()) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool EntryRepository::RecordSentenceCommit(const std::string& eventId, uint64_t sessionId, uint64_t contextId,
                                          InputScheme scheme, const std::string& scopedLearningCode,
                                          const std::wstring& text, const std::string& storagePinyinCode,
                                          const std::string& originalCandidateId,
                                          const std::vector<std::pair<InputScheme, std::string>>& allCodes,
                                          bool learningEnabled) {
    if (eventId.empty() || text.empty() || storagePinyinCode.empty()) return false;
    int64_t now = m_clock ? m_clock->Now() : 0;

    std::string lowerScoped = scopedLearningCode;
    std::transform(lowerScoped.begin(), lowerScoped.end(), lowerScoped.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string lowerPinyin = storagePinyinCode;
    std::transform(lowerPinyin.begin(), lowerPinyin.end(), lowerPinyin.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string currentFingerprint = BuildSentencePayloadFingerprint(
        sessionId, contextId, scheme, lowerScoped, text, lowerPinyin, originalCandidateId);

    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    // 1. Existing commit_events check (idempotency & immutable payload verification)
    auto stmtEventCheck = m_db.Prepare("SELECT session_id, context_id, scheme, code, entry_id, status, payload_hash FROM commit_events WHERE event_id = ?;");
    if (!stmtEventCheck) {
        tx.Rollback();
        return false;
    }
    stmtEventCheck->BindText(1, eventId);
    if (stmtEventCheck->Step()) {
        uint64_t prevSid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(0));
        uint64_t prevCid = static_cast<uint64_t>(stmtEventCheck->ColumnInt64(1));
        int prevScheme = stmtEventCheck->ColumnInt(2);
        std::string prevCode = stmtEventCheck->ColumnText(3);
        std::string prevEntryId = stmtEventCheck->ColumnText(4);
        int prevStatus = stmtEventCheck->ColumnInt(5);
        std::string prevPayload = stmtEventCheck->ColumnText(6);

        if (prevStatus == 2) {
            tx.Rollback();
            return false; // Cancelled event can NEVER become Recorded!
        }
        if (sessionId != prevSid || contextId != prevCid) {
            tx.Rollback();
            return false; // Owner mismatch
        }

        if (!prevPayload.empty()) {
            if (prevPayload == currentFingerprint) {
                return true; // Idempotent replay of exact identical sentence commit
            }
            tx.Rollback();
            return false; // Conflict payload replay rejected!
        }

        // Legacy compatibility for older events without payload_hash
        if (prevScheme == static_cast<int>(scheme) && prevCode == lowerScoped) {
            return true;
        }
        tx.Rollback();
        return false; // Conflict payload replay rejected
    }
    if (stmtEventCheck->HasError()) {
        tx.Rollback();
        return false;
    }

    // 2. Strict pending_commits snapshot verification (Normal ACK must have trusted pending)
    auto stmtPending = m_db.Prepare(
        "SELECT session_id, context_id, scheme, query_code, entry_id, text, global_epoch, entry_epoch, layout_id, matched_code, resolved_code "
        "FROM pending_commits WHERE event_id = ?;"
    );
    if (!stmtPending) {
        tx.Rollback();
        return false;
    }
    stmtPending->BindText(1, eventId);
    if (!stmtPending->Step()) {
        if (stmtPending->HasError()) {
            tx.Rollback();
            return false;
        }
        tx.Rollback();
        return false; // Unknown event has no pending: fail-closed!
    }

    uint64_t pendSid = static_cast<uint64_t>(stmtPending->ColumnInt64(0));
    uint64_t pendCid = static_cast<uint64_t>(stmtPending->ColumnInt64(1));
    int pendScheme = stmtPending->ColumnInt(2);
    std::string pendQueryCode = stmtPending->ColumnText(3);
    std::string pendEntryId = stmtPending->ColumnText(4);
    std::wstring pendText = Utf8ToWide(stmtPending->ColumnText(5));
    int64_t pendGlobalEpoch = stmtPending->ColumnInt64(6);
    int64_t pendEntryEpoch = stmtPending->ColumnInt64(7);
    std::string pendLayoutId = stmtPending->ColumnText(8);
    std::string pendMatchedCode = stmtPending->ColumnText(9);
    std::string pendResolvedCode = stmtPending->ColumnText(10);

    if (pendSid != sessionId || pendCid != contextId) {
        tx.Rollback();
        return false;
    }
    if (pendScheme != static_cast<int>(scheme) || pendText != text ||
        (!originalCandidateId.empty() && pendEntryId != originalCandidateId)) {
        tx.Rollback();
        return false; // Snapshot mismatch/tampering rejected
    }

    // P1-1: Verify scopedLearningCode against trusted pending
    std::string lowerQuery = pendQueryCode;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](unsigned char c) { return std::tolower(c); });

    std::string expectedScoped;
    if (scheme == InputScheme::Shuangpin) {
        if (!pendLayoutId.empty()) {
            ShuangpinLayout lay = ShuangpinLayoutFromString(pendLayoutId);
            std::string canonical = ShuangpinLayoutToString(lay);
            expectedScoped = canonical + ":" + lowerQuery;
        } else {
            expectedScoped = lowerQuery;
        }
        if (lowerScoped != expectedScoped) {
            tx.Rollback();
            return false; // Scoped code mismatch for Shuangpin
        }
    } else if (scheme == InputScheme::Pinyin) {
        std::string strippedQuery;
        for (char ch : lowerQuery) {
            if (ch != '\'' && ch != ' ') strippedQuery.push_back(ch);
        }
        if (lowerScoped != lowerQuery && lowerScoped != strippedQuery) {
            tx.Rollback();
            return false; // Scoped code mismatch for Pinyin
        }
    } else {
        if (lowerScoped != lowerQuery) {
            tx.Rollback();
            return false;
        }
    }

    // P1-1: Verify storagePinyinCode against trusted pending facts
    std::string pinyinSource;
    if (scheme == InputScheme::Shuangpin && !pendResolvedCode.empty()) {
        pinyinSource = pendResolvedCode;
    } else if (!pendQueryCode.empty()) {
        pinyinSource = pendQueryCode;
    } else {
        pinyinSource = pendMatchedCode;
    }
    std::string expectedStoragePinyin;
    for (char ch : pinyinSource) {
        if (ch != '\'' && ch != ' ') {
            expectedStoragePinyin.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (lowerPinyin != expectedStoragePinyin) {
        tx.Rollback();
        return false; // Storage pinyin code mismatch
    }

    // If learning is disabled, delete pending and record commit event with payload_hash
    if (!learningEnabled) {
        auto stmtDelPending = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
        if (!stmtDelPending) {
            tx.Rollback();
            return false;
        }
        stmtDelPending->BindText(1, eventId);
        if (!stmtDelPending->Execute()) {
            tx.Rollback();
            return false;
        }

        auto stmtEventInsert = m_db.Prepare("INSERT INTO commit_events (event_id, session_id, context_id, scheme, code, entry_id, committed_at, status, payload_hash) VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?);");
        if (!stmtEventInsert) {
            tx.Rollback();
            return false;
        }
        stmtEventInsert->BindText(1, eventId);
        stmtEventInsert->BindInt64(2, static_cast<int64_t>(sessionId));
        stmtEventInsert->BindInt64(3, static_cast<int64_t>(contextId));
        stmtEventInsert->BindInt(4, static_cast<int>(scheme));
        stmtEventInsert->BindText(5, lowerScoped);
        stmtEventInsert->BindText(6, pendEntryId);
        stmtEventInsert->BindInt64(7, now);
        stmtEventInsert->BindText(8, currentFingerprint);
        if (!stmtEventInsert->Execute()) {
            tx.Rollback();
            return false;
        }
        return tx.Commit();
    }

    // 3. Atomic Find or Create User Entry within THIS transaction
    std::string targetId;
    std::string textUtf8 = WideToUtf8(text);
    const char* sqlFind = R"SQL(
        SELECT e.id FROM entries e
        JOIN entry_codes ec ON e.id = ec.entry_id
        WHERE e.text = ? AND ec.scheme = 1 AND ec.code = ?
        LIMIT 1;
    )SQL";
    auto stmtFind = m_db.Prepare(sqlFind);
    if (!stmtFind) {
        tx.Rollback();
        return false;
    }
    stmtFind->BindText(1, textUtf8);
    stmtFind->BindText(2, lowerPinyin);
    if (stmtFind->Step()) {
        targetId = stmtFind->ColumnText(0);
    }
    if (stmtFind->HasError()) {
        tx.Rollback();
        return false;
    }

    bool isNewEntryCreated = false;
    if (targetId.empty()) {
        // Create new user entry atomically
        targetId = GenerateUniqueId();
        auto stmtInsertEntry = m_db.Prepare("INSERT INTO entries (id, text, source, created_at, updated_at) VALUES (?, ?, ?, ?, ?);");
        if (!stmtInsertEntry) {
            tx.Rollback();
            return false;
        }
        stmtInsertEntry->BindText(1, targetId);
        stmtInsertEntry->BindText(2, textUtf8);
        stmtInsertEntry->BindInt(3, static_cast<int>(EntrySource::User));
        stmtInsertEntry->BindInt64(4, now);
        stmtInsertEntry->BindInt64(5, now);
        if (!stmtInsertEntry->Execute()) {
            tx.Rollback();
            return false;
        }

        auto stmtInsertCode = m_db.Prepare("INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at) VALUES (?, ?, ?, ?);");
        if (!stmtInsertCode) {
            tx.Rollback();
            return false;
        }
        for (const auto& codePair : allCodes) {
            if (codePair.second.empty()) continue;
            std::string cLower = codePair.second;
            std::transform(cLower.begin(), cLower.end(), cLower.begin(), [](unsigned char ch) { return std::tolower(ch); });
            stmtInsertCode->Reset();
            stmtInsertCode->BindText(1, targetId);
            stmtInsertCode->BindInt(2, static_cast<int>(codePair.first));
            stmtInsertCode->BindText(3, cLower);
            stmtInsertCode->BindInt64(4, now);
            if (!stmtInsertCode->Execute()) {
                tx.Rollback();
                return false;
            }
        }
        isNewEntryCreated = true;
    }

    // 4. Reset Epoch Verification across identities
    bool shouldUpdateStats = true;
    int64_t curGlobalEpoch = 1;
    auto stmtGlobalEpoch = m_db.Prepare("SELECT value FROM meta_settings WHERE key = 'global_stats_reset_epoch';");
    if (!stmtGlobalEpoch) {
        tx.Rollback();
        return false;
    }
    if (stmtGlobalEpoch->Step()) {
        try {
            curGlobalEpoch = std::stoll(stmtGlobalEpoch->ColumnText(0));
        } catch (...) {
            curGlobalEpoch = 1;
        }
    } else if (stmtGlobalEpoch->HasError()) {
        tx.Rollback();
        return false;
    }
    if (pendGlobalEpoch > 0 && pendGlobalEpoch < curGlobalEpoch) {
        shouldUpdateStats = false; // Global stats reset occurred while pending
    }
    if (!isNewEntryCreated) {
        int64_t curEntryEpoch = 1;
        auto stmtEntryEpoch = m_db.Prepare("SELECT reset_epoch FROM entry_resets WHERE entry_id = ?;");
        if (!stmtEntryEpoch) {
            tx.Rollback();
            return false;
        }
        stmtEntryEpoch->BindText(1, targetId);
        if (stmtEntryEpoch->Step()) {
            curEntryEpoch = stmtEntryEpoch->ColumnInt64(0);
        } else if (stmtEntryEpoch->HasError()) {
            tx.Rollback();
            return false;
        }
        if (pendEntryEpoch > 0 && pendEntryEpoch < curEntryEpoch) {
            shouldUpdateStats = false; // Target entry reset occurred while pending
        }
    }

    // 5. Update stats if valid
    if (shouldUpdateStats) {
        auto statOpt = GetStat(scheme, lowerScoped, targetId);
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
        stmtStat->BindText(2, lowerScoped);
        stmtStat->BindText(3, targetId);
        stmtStat->BindInt(4, newCount);
        stmtStat->BindInt64(5, now);
        stmtStat->BindDouble(6, newEff);
        stmtStat->BindInt64(7, now);
        if (!stmtStat->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    // 6. Delete pending commit
    auto stmtDelPending = m_db.Prepare("DELETE FROM pending_commits WHERE event_id = ?;");
    if (!stmtDelPending) {
        tx.Rollback();
        return false;
    }
    stmtDelPending->BindText(1, eventId);
    if (!stmtDelPending->Execute()) {
        tx.Rollback();
        return false;
    }

    // 7. Insert commit_event using INSERT (NOT REPLACE) with immutable payload_hash
    auto stmtEventInsert = m_db.Prepare("INSERT INTO commit_events (event_id, session_id, context_id, scheme, code, entry_id, committed_at, status, payload_hash) VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?);");
    if (!stmtEventInsert) {
        tx.Rollback();
        return false;
    }
    stmtEventInsert->BindText(1, eventId);
    stmtEventInsert->BindInt64(2, static_cast<int64_t>(sessionId));
    stmtEventInsert->BindInt64(3, static_cast<int64_t>(contextId));
    stmtEventInsert->BindInt(4, static_cast<int>(scheme));
    stmtEventInsert->BindText(5, lowerScoped);
    stmtEventInsert->BindText(6, targetId);
    stmtEventInsert->BindInt64(7, now);
    stmtEventInsert->BindText(8, currentFingerprint);
    if (!stmtEventInsert->Execute()) {
        tx.Rollback();
        return false;
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
    if (!tx.IsStarted()) return false;
    int64_t finalLastUsed = lastUsed;
    double finalEff = effCount;
    int64_t finalDecayAt = lastDecayAt;

    if (mergeMode) {
        auto existing = GetStat(scheme, lowerCode, entryId);
        if (existing) {
            finalCount = (std::max)(existing->selected_count, count);
            finalLastUsed = (std::max)(existing->last_selected_at, lastUsed);

            // Compare both effective counts on a unified timeline:
            int64_t T = (std::max)(existing->last_decay_at, lastDecayAt);
            double effExisting = CalculateDecayedCount(existing->effective_count, existing->last_decay_at, T);
            double effIncoming = CalculateDecayedCount(effCount, lastDecayAt, T);
            finalEff = (std::max)(effExisting, effIncoming);
            finalDecayAt = T;
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
    if (!stmt) throw std::runtime_error("Cannot read backup statistics");
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
    if (stmt->HasError()) throw std::runtime_error("Backup statistic read failed");
    return list;
}

std::vector<EntryStat> EntryRepository::GetActiveStats(InputScheme scheme, const std::string& code) {
    std::vector<EntryStat> list;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT scheme, code, entry_id, selected_count, last_selected_at, effective_count, last_decay_at FROM entry_stats WHERE scheme = ? AND code = ? AND selected_count > 0;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("GetActiveStats prepare failed: " + m_db.GetLastError());
        }
        return list;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    while (stmt->Step()) {
        EntryStat s;
        s.scheme = scheme;
        s.code = lowerCode;
        s.entry_id = stmt->ColumnText(2);
        s.selected_count = stmt->ColumnInt(3);
        s.last_selected_at = stmt->ColumnInt64(4);
        s.effective_count = stmt->ColumnDouble(5);
        s.last_decay_at = stmt->ColumnInt64(6);
        list.push_back(std::move(s));
    }
    if (stmt->HasError()) {
        throw std::runtime_error("GetActiveStats step failed: " + stmt->LastErrorMessage());
    }
    return list;
}

std::optional<EntryStat> EntryRepository::GetStat(InputScheme scheme, const std::string& code, const std::string& entryId) {
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    auto stmt = m_db.Prepare("SELECT selected_count, last_selected_at, effective_count, last_decay_at FROM entry_stats WHERE scheme = ? AND code = ? AND entry_id = ?;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("GetStat prepare failed: " + m_db.GetLastError());
        }
        return std::nullopt;
    }
    stmt->BindInt(1, static_cast<int>(scheme));
    stmt->BindText(2, lowerCode);
    stmt->BindText(3, entryId);
    if (!stmt->Step()) {
        if (stmt->HasError()) {
            throw std::runtime_error("GetStat step failed: " + stmt->LastErrorMessage());
        }
        return std::nullopt;
    }

    EntryStat s;
    s.selected_count = stmt->ColumnInt(0);
    s.last_selected_at = stmt->ColumnInt64(1);
    s.effective_count = stmt->ColumnDouble(2);
    s.last_decay_at = stmt->ColumnInt64(3);
    return s;
}

int64_t EntryRepository::GetGlobalStatsResetEpoch() {
    auto val = GetMetaSetting("global_stats_reset_epoch");
    if (!val || val->empty()) return 1;
    try {
        return std::stoll(*val);
    } catch (...) {
        return 1;
    }
}

int64_t EntryRepository::GetEntryStatsResetEpoch(const std::string& entryId) {
    if (entryId.empty()) return 1;
    auto stmt = m_db.Prepare("SELECT reset_epoch FROM entry_resets WHERE entry_id = ?;");
    if (!stmt) return 1;
    stmt->BindText(1, entryId);
    if (stmt->Step()) {
        return stmt->ColumnInt64(0);
    }
    return 1;
}

bool EntryRepository::ResetStats(const std::string& entryId) {
    if (entryId.empty()) {
        return ResetAllStats();
    }
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    auto stmt = m_db.Prepare("DELETE FROM entry_stats WHERE entry_id = ?;");
    if (!stmt) {
        tx.Rollback();
        return false;
    }
    stmt->BindText(1, entryId);
    if (!stmt->Execute()) {
        tx.Rollback();
        return false;
    }

    int64_t curEpoch = GetEntryStatsResetEpoch(entryId);
    auto stmtEpoch = m_db.Prepare("INSERT OR REPLACE INTO entry_resets (entry_id, reset_epoch) VALUES (?, ?);");
    if (!stmtEpoch) {
        tx.Rollback();
        return false;
    }
    stmtEpoch->BindText(1, entryId);
    stmtEpoch->BindInt64(2, curEpoch + 1);
    if (!stmtEpoch->Execute()) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool EntryRepository::EnsureBuiltinEntryExists(const std::string& entryId, const std::wstring& text) {
    if (entryId.empty()) return false;
    auto stmt = m_db.Prepare("INSERT INTO entries (id, text, source, created_at, updated_at) VALUES (?, ?, 0, ?, ?) ON CONFLICT(id) DO UPDATE SET text = CASE WHEN entries.text = '' THEN excluded.text ELSE entries.text END;");
    if (!stmt) return false;
    int64_t now = m_clock->Now();
    stmt->BindText(1, entryId);
    stmt->BindText(2, WideToUtf8(text));
    stmt->BindInt64(3, now);
    stmt->BindInt64(4, now);
    return stmt->Execute();
}

bool EntryRepository::ResetAllStats() {
    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;
    if (!m_db.Execute("DELETE FROM entry_stats;")) {
        tx.Rollback();
        return false;
    }
    if (!m_db.Execute("DELETE FROM entry_resets;")) {
        tx.Rollback();
        return false;
    }
    int64_t curEpoch = GetGlobalStatsResetEpoch();
    if (!SetMetaSetting("global_stats_reset_epoch", std::to_string(curEpoch + 1))) {
        tx.Rollback();
        return false;
    }
    int64_t now = m_clock ? m_clock->Now() : 0;
    SetLastResetStatsAt(now);
    return tx.Commit();
}

int64_t EntryRepository::GetLastResetStatsAt() {
    auto val = GetMetaSetting("last_reset_stats_at");
    if (!val || val->empty()) return 0;
    try {
        return std::stoll(*val);
    } catch (...) {
        return 0;
    }
}

void EntryRepository::SetLastResetStatsAt(int64_t ts) {
    SetMetaSetting("last_reset_stats_at", std::to_string(ts));
}

bool EntryRepository::ReorderPinnedEntries(InputScheme scheme, const std::string& code, const std::vector<std::string>& entryIds,
                                          std::optional<uint64_t> expectedRevision, uint64_t* outRevision, bool* outConflict) {
    if (outConflict) *outConflict = false;
    if (code.empty()) return false;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });

    // Validate entryIds: no empty IDs, no duplicates
    std::set<std::string> seenIds;
    for (const auto& eid : entryIds) {
        if (eid.empty()) return false;
        if (!seenIds.insert(eid).second) return false;
    }

    SqliteDb::Transaction tx(m_db);
    if (!tx.IsStarted()) return false;

    // Builtin entry materialization inside transaction
    for (const auto& eid : entryIds) {
        if (eid.rfind("b_", 0) == 0) {
            if (!m_builtinDict) {
                tx.Rollback();
                return false;
            }
            auto textOpt = m_builtinDict->GetTextById(eid);
            if (!textOpt || textOpt->empty()) {
                tx.Rollback();
                return false;
            }
            if (!EnsureBuiltinEntryExists(eid, *textOpt)) {
                tx.Rollback();
                return false;
            }
        }
    }

    uint64_t curRev = GetPinnedRevision(scheme, lowerCode);
    if (expectedRevision.has_value() && *expectedRevision != curRev) {
        if (outConflict) *outConflict = true;
        tx.Rollback();
        return false;
    }

    int64_t now = m_clock ? m_clock->Now() : 0;

    // Unpin existing items for this code
    auto stmtUnpin = m_db.Prepare("UPDATE sort_preferences SET is_pinned = 0, order_rank = 0, updated_at = ? WHERE scheme = ? AND code = ?;");
    if (!stmtUnpin) {
        tx.Rollback();
        return false;
    }
    stmtUnpin->BindInt64(1, now);
    stmtUnpin->BindInt(2, static_cast<int>(scheme));
    stmtUnpin->BindText(3, lowerCode);
    if (!stmtUnpin->Execute()) {
        tx.Rollback();
        return false;
    }

    const char* sqlUpsert = R"SQL(
        INSERT INTO sort_preferences (scheme, code, entry_id, is_pinned, order_rank, updated_at)
        VALUES (?, ?, ?, 1, ?, ?)
        ON CONFLICT(scheme, code, entry_id) DO UPDATE SET
            is_pinned = 1,
            order_rank = excluded.order_rank,
            updated_at = excluded.updated_at;
    )SQL";
    auto stmtUpsert = m_db.Prepare(sqlUpsert);
    if (!stmtUpsert) {
        tx.Rollback();
        return false;
    }

    for (size_t i = 0; i < entryIds.size(); ++i) {
        stmtUpsert->Reset();
        stmtUpsert->BindInt(1, static_cast<int>(scheme));
        stmtUpsert->BindText(2, lowerCode);
        stmtUpsert->BindText(3, entryIds[i]);
        stmtUpsert->BindInt(4, static_cast<int>(i + 1));
        stmtUpsert->BindInt64(5, now);
        if (!stmtUpsert->Execute()) {
            tx.Rollback();
            return false;
        }
    }

    uint64_t nextRev = curRev + 1;
    if (!SetPinnedRevision(scheme, lowerCode, nextRev)) {
        tx.Rollback();
        return false;
    }
    if (outRevision) *outRevision = nextRev;

    return tx.Commit();
}

uint64_t EntryRepository::GetPinnedRevision(InputScheme scheme, const std::string& code) {
    if (code.empty()) return 1;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string key = "pin_rev:" + SchemeToString(scheme) + ":" + lowerCode;
    auto val = GetMetaSetting(key);
    if (!val || val->empty()) return 1;
    try {
        return std::stoull(*val);
    } catch (...) {
        return 1;
    }
}

bool EntryRepository::SetPinnedRevision(InputScheme scheme, const std::string& code, uint64_t revision) {
    if (code.empty()) return false;
    std::string lowerCode = code;
    std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string key = "pin_rev:" + SchemeToString(scheme) + ":" + lowerCode;
    return SetMetaSetting(key, std::to_string(revision));
}

bool EntryRepository::SetMetaSetting(const std::string& key, const std::string& value) {
    if (key.empty()) return false;
    auto stmt = m_db.Prepare("INSERT INTO meta_settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!stmt) return false;
    stmt->BindText(1, key);
    stmt->BindText(2, value);
    return stmt->Execute();
}

std::optional<std::string> EntryRepository::GetMetaSetting(const std::string& key) {
    if (key.empty()) return std::nullopt;
    auto stmt = m_db.Prepare("SELECT value FROM meta_settings WHERE key = ?;");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, key);
    if (stmt->Step()) {
        return stmt->ColumnText(0);
    }
    return std::nullopt;
}

bool EntryRepository::HasPendingCommitsForEntry(const std::string& entryId) {
    if (entryId.empty()) return false;
    auto stmt = m_db.Prepare("SELECT 1 FROM pending_commits WHERE entry_id = ? LIMIT 1;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("HasPendingCommitsForEntry prepare failed: " + m_db.GetLastError());
        }
        return false;
    }
    stmt->BindText(1, entryId);
    bool has = stmt->Step();
    if (stmt->HasError()) {
        throw std::runtime_error("HasPendingCommitsForEntry step failed: " + stmt->LastErrorMessage());
    }
    return has;
}

bool EntryRepository::HasAnyPendingCommits() {
    auto stmt = m_db.Prepare("SELECT 1 FROM pending_commits LIMIT 1;");
    if (!stmt) {
        if (m_db.GetLastErrorCode() != SQLITE_OK && m_db.GetLastErrorCode() != SQLITE_DONE) {
            throw std::runtime_error("HasAnyPendingCommits prepare failed: " + m_db.GetLastError());
        }
        return false;
    }
    bool has = stmt->Step();
    if (stmt->HasError()) {
        throw std::runtime_error("HasAnyPendingCommits step failed: " + stmt->LastErrorMessage());
    }
    return has;
}

} // namespace ime

