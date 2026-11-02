#pragma once

#include "../types.h"
#include "../clock.h"
#include "../storage/sqlite_db.h"
#include "../storage/entry_repository.h"
#include "../storage/json_backup.h"
#include "../encoding/pinyin_segmenter.h"
#include "../encoding/wubi_encoder.h"
#include "../encoding/custom_encoder.h"
#include "../dictionary/builtin_dict.h"
#include "../ranking/ranker.h"
#include <memory>
#include <string>
#include <vector>

namespace ime {

class ImeEngine {
public:
    ImeEngine();
    ~ImeEngine();

    bool Initialize(const std::string& dbPath,
                    const std::string& dictDir,
                    std::shared_ptr<IClock> clock = nullptr);

    QueryResult Query(InputScheme scheme, const std::string& code);

    bool CommitCandidate(const std::string& eventId,
                         InputScheme scheme,
                         const std::string& code,
                         const std::string& entryId);

    // User Dictionary Management
    std::string AddUserEntry(const std::wstring& text,
                            const std::vector<std::pair<InputScheme, std::string>>& codes);
    bool EditUserEntry(const std::string& entryId, const std::wstring& newText);
    bool DeleteUserEntry(const std::string& entryId);
    std::optional<Entry> GetUserEntry(const std::string& entryId);
    std::vector<Entry> GetAllUserEntries();

    // Pinning & Ordering
    bool PinCandidate(InputScheme scheme, const std::string& code, const std::string& entryId, int rank = 1);
    bool UnpinCandidate(InputScheme scheme, const std::string& code, const std::string& entryId);

    // Stats & Backup
    bool ResetStats(const std::string& entryId = "");
    bool ExportData(const std::string& jsonFilePath, std::string* errorOut = nullptr);
    bool ImportData(const std::string& jsonFilePath, bool replaceMode = false, std::string* errorOut = nullptr);

    std::vector<std::string> ExplainCandidates(InputScheme scheme, const std::string& code);

    std::shared_ptr<IClock> GetClock() const { return m_clock; }
    const BuiltinDictionary& GetBuiltinDict() const { return m_builtinDict; }

private:
    std::shared_ptr<IClock> m_clock;
    SqliteDb m_db;
    std::unique_ptr<EntryRepository> m_repo;
    PinyinSegmenter m_pinyinSegmenter;
    BuiltinDictionary m_builtinDict;
    std::unique_ptr<Ranker> m_ranker;
    bool m_initialized = false;
};

} // namespace ime

