#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "sqlite_db.h"

namespace ime {

struct EmojiMappingRecord {
    int64_t id = 0;
    std::string trigger_type; // "code" or "text"
    std::string trigger_key;  // lowercase encoding or exact candidate text (UTF-8)
    std::string emoji;        // UTF-8 emoji sequence
    std::string label;        // optional note/description
    int priority = 100;
    bool enabled = true;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

class EmojiRepository {
public:
    explicit EmojiRepository(SqliteDb& db);

    uint64_t GetRevision() const { return m_revision.load(std::memory_order_acquire); }
    void BumpRevision() { m_revision.fetch_add(1, std::memory_order_acq_rel); }

    bool GetAllMappings(std::vector<EmojiMappingRecord>& outMappings, bool onlyEnabled = false);
    bool GetMappingById(int64_t id, EmojiMappingRecord& outMapping);
    bool AddMapping(const EmojiMappingRecord& rec, int64_t& outId, std::string* errorOut = nullptr);
    bool UpdateMapping(const EmojiMappingRecord& rec, std::string* errorOut = nullptr);
    bool DeleteMapping(int64_t id);

    // Explicit matching:
    // 1. Explicit code match (trigger_type='code', exact lowercase rawCode)
    // 2. Top normal candidate exact text match (trigger_type='text', exact topCandidateTextUtf8)
    // Deduplicated by emoji sequence, preserving priority order.
    bool MatchForQuery(const std::string& rawCode,
                       const std::string& topCandidateTextUtf8,
                       std::vector<EmojiMappingRecord>& outMatched);

private:
    SqliteDb& m_db;
    std::atomic<uint64_t> m_revision{1};
};

} // namespace ime
