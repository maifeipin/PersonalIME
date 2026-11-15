#include "emoji_repository.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ime {

namespace {
std::string NormalizeCodeKey(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool ValidateRecord(const EmojiMappingRecord& rec, std::string& normKey, std::string* errorOut) {
    if (rec.trigger_type != "code" && rec.trigger_type != "text") {
        if (errorOut) *errorOut = "trigger_type must be 'code' or 'text'";
        return false;
    }
    if (rec.trigger_key.empty()) {
        if (errorOut) *errorOut = "trigger_key cannot be empty";
        return false;
    }
    if (rec.emoji.empty()) {
        if (errorOut) *errorOut = "emoji cannot be empty";
        return false;
    }
    normKey = (rec.trigger_type == "code") ? NormalizeCodeKey(rec.trigger_key) : rec.trigger_key;
    return true;
}

EmojiMappingRecord ReadRow(Statement& stmt) {
    EmojiMappingRecord rec;
    rec.id = stmt.ColumnInt64(0);
    rec.trigger_type = stmt.ColumnText(1);
    rec.trigger_key = stmt.ColumnText(2);
    rec.emoji = stmt.ColumnText(3);
    rec.label = stmt.ColumnText(4);
    rec.priority = stmt.ColumnInt(5);
    rec.enabled = (stmt.ColumnInt(6) != 0);
    rec.created_at = stmt.ColumnInt64(7);
    rec.updated_at = stmt.ColumnInt64(8);
    return rec;
}
} // namespace

EmojiRepository::EmojiRepository(SqliteDb& db) : m_db(db) {}

bool EmojiRepository::GetAllMappings(std::vector<EmojiMappingRecord>& outMappings, bool onlyEnabled) {
    outMappings.clear();
    const char* sql = onlyEnabled
        ? "SELECT id, trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at "
          "FROM emoji_mappings WHERE enabled = 1 ORDER BY priority DESC, id ASC;"
        : "SELECT id, trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at "
          "FROM emoji_mappings ORDER BY trigger_type ASC, priority DESC, id ASC;";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) return false;
    while (stmt->Step()) {
        outMappings.push_back(ReadRow(*stmt));
    }
    return !stmt->HasError();
}

bool EmojiRepository::GetMappingById(int64_t id, EmojiMappingRecord& outMapping) {
    auto stmt = m_db.Prepare(
        "SELECT id, trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at "
        "FROM emoji_mappings WHERE id = ?;");
    if (!stmt) return false;
    stmt->BindInt64(1, id);
    if (stmt->Step()) {
        outMapping = ReadRow(*stmt);
        return true;
    }
    return false;
}

bool EmojiRepository::AddMapping(const EmojiMappingRecord& rec, int64_t& outId, std::string* errorOut) {
    std::string normKey;
    if (!ValidateRecord(rec, normKey, errorOut)) {
        return false;
    }

    auto stmt = m_db.Prepare(
        "INSERT INTO emoji_mappings (trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'), strftime('%s','now'));");
    if (!stmt) {
        if (errorOut) *errorOut = "Failed to prepare insert statement";
        return false;
    }
    stmt->BindText(1, rec.trigger_type);
    stmt->BindText(2, normKey);
    stmt->BindText(3, rec.emoji);
    stmt->BindText(4, rec.label);
    stmt->BindInt(5, rec.priority);
    stmt->BindInt(6, rec.enabled ? 1 : 0);
    if (!stmt->Execute()) {
        if (errorOut) *errorOut = "Failed to insert emoji mapping (possible duplicate trigger/emoji)";
        return false;
    }
    outId = m_db.LastInsertRowId();
    BumpRevision();
    return true;
}

bool EmojiRepository::UpdateMapping(const EmojiMappingRecord& rec, std::string* errorOut) {
    if (rec.id <= 0) {
        if (errorOut) *errorOut = "id must be > 0";
        return false;
    }
    std::string normKey;
    if (!ValidateRecord(rec, normKey, errorOut)) {
        return false;
    }

    auto stmt = m_db.Prepare(
        "UPDATE emoji_mappings "
        "SET trigger_type = ?, trigger_key = ?, emoji = ?, label = ?, priority = ?, enabled = ?, updated_at = strftime('%s','now') "
        "WHERE id = ?;");
    if (!stmt) {
        if (errorOut) *errorOut = "Failed to prepare update statement";
        return false;
    }
    stmt->BindText(1, rec.trigger_type);
    stmt->BindText(2, normKey);
    stmt->BindText(3, rec.emoji);
    stmt->BindText(4, rec.label);
    stmt->BindInt(5, rec.priority);
    stmt->BindInt(6, rec.enabled ? 1 : 0);
    stmt->BindInt64(7, rec.id);
    if (!stmt->Execute()) {
        if (errorOut) *errorOut = "Failed to update emoji mapping (possible duplicate trigger/emoji)";
        return false;
    }
    if (m_db.Changes() == 0) {
        if (errorOut) *errorOut = "Emoji mapping not found";
        return false;
    }
    BumpRevision();
    return true;
}

bool EmojiRepository::DeleteMapping(int64_t id) {
    auto stmt = m_db.Prepare("DELETE FROM emoji_mappings WHERE id = ?;");
    if (!stmt) return false;
    stmt->BindInt64(1, id);
    if (!stmt->Execute()) return false;
    if (m_db.Changes() == 0) return false;
    BumpRevision();
    return true;
}

bool EmojiRepository::MatchForQuery(const std::string& rawCode,
                                    const std::string& topCandidateTextUtf8,
                                    std::vector<EmojiMappingRecord>& outMatched) {
    outMatched.clear();
    std::unordered_set<std::string> seenEmojis;

    // 1. Explicit encoding match first
    std::string normCode = NormalizeCodeKey(rawCode);
    if (!normCode.empty()) {
        auto stmt = m_db.Prepare(
            "SELECT id, trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at "
            "FROM emoji_mappings "
            "WHERE enabled = 1 AND trigger_type = 'code' AND trigger_key = ? "
            "ORDER BY priority DESC, id ASC;");
        if (!stmt) return false;
        stmt->BindText(1, normCode);
        while (stmt->Step()) {
            EmojiMappingRecord rec = ReadRow(*stmt);
            if (seenEmojis.insert(rec.emoji).second) {
                outMatched.push_back(std::move(rec));
            }
        }
        if (stmt->HasError()) return false;
    }

    // 2. Top normal candidate exact text match second
    if (!topCandidateTextUtf8.empty()) {
        auto stmt = m_db.Prepare(
            "SELECT id, trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at "
            "FROM emoji_mappings "
            "WHERE enabled = 1 AND trigger_type = 'text' AND trigger_key = ? "
            "ORDER BY priority DESC, id ASC;");
        if (!stmt) return false;
        stmt->BindText(1, topCandidateTextUtf8);
        while (stmt->Step()) {
            EmojiMappingRecord rec = ReadRow(*stmt);
            if (seenEmojis.insert(rec.emoji).second) {
                outMatched.push_back(std::move(rec));
            }
        }
        if (stmt->HasError()) return false;
    }

    return true;
}

} // namespace ime
