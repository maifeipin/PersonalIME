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

bool CountUtf16UnitsStrict(const std::string& utf8, size_t& outUnits) {
    outUnits = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8.data());
    size_t i = 0, n = utf8.size();
    while (i < n) {
        unsigned char c = p[i];
        uint32_t cp = 0;
        size_t seqLen = 0;
        if (c == 0) {
            return false; // Embedded NUL forbidden
        } else if (c < 0x80) {
            cp = c;
            seqLen = 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return false;
            cp = ((c & 0x1F) << 6) | (p[i + 1] & 0x3F);
            if (cp < 0x80) return false;
            seqLen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false;
            cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            seqLen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return false;
            cp = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) | ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return false;
            seqLen = 4;
        } else {
            return false;
        }
        outUnits += (cp > 0xFFFF) ? 2 : 1;
        i += seqLen;
    }
    return true;
}

bool ValidateRecord(const EmojiMappingRecord& rec, std::string& normKey, std::string* errorOut) {
    if (rec.trigger_type != "code" && rec.trigger_type != "text") {
        if (errorOut) *errorOut = "trigger_type must be 'code' or 'text'";
        return false;
    }
    if (rec.trigger_key.empty() || rec.trigger_key.find('\0') != std::string::npos) {
        if (errorOut) *errorOut = "trigger_key cannot be empty or contain NUL";
        return false;
    }
    if (rec.emoji.empty() || rec.emoji.find('\0') != std::string::npos) {
        if (errorOut) *errorOut = "emoji cannot be empty or contain NUL";
        return false;
    }
    size_t utf16Units = 0;
    if (!CountUtf16UnitsStrict(rec.emoji, utf16Units) || utf16Units == 0 ||
        utf16Units > EmojiRepository::kMaxEmojiUtf16Units) {
        if (errorOut) {
            *errorOut = "emoji must be valid UTF-8 and at most " +
                        std::to_string(EmojiRepository::kMaxEmojiUtf16Units) + " UTF-16 code units";
        }
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

bool EmojiRepository::IsAssociationEnabled() {
    auto stmt = m_db.Prepare("SELECT value FROM meta_settings WHERE key = 'emoji_association_enabled';");
    if (!stmt) return true;
    if (stmt->Step()) {
        return stmt->ColumnText(0) != "0";
    }
    return true;
}

bool EmojiRepository::SetAssociationEnabled(bool enabled) {
    auto stmt = m_db.Prepare("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('emoji_association_enabled', ?);");
    if (!stmt) return false;
    stmt->BindText(1, enabled ? "1" : "0");
    if (!stmt->Execute()) return false;
    BumpRevision();
    return true;
}

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
