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
#include "../strategy/input_coordinator.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <mutex>

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
                         uint64_t sessionId,
                         uint64_t contextId,
                         InputScheme scheme,
                         const std::string& code,
                         const std::string& entryId);
    bool CommitCandidate(const std::string& eventId,
                         InputScheme scheme,
                         const std::string& code,
                         const std::string& entryId) {
        return CommitCandidate(eventId, 0, 0, scheme, code, entryId);
    }
    bool RecordCancel(const std::string& eventId, uint64_t sessionId, uint64_t contextId);
    EntryRepository::CommitEventRecord GetCommitEventInfo(const std::string& eventId);
    bool IsCommitRecorded(const std::string& eventId);

    // Persistent Pending Commits across service crashes / restarts
    bool SavePendingCommit(const std::string& eventId,
                           uint64_t sessionId,
                           uint64_t contextId,
                           InputScheme scheme,
                           const std::string& queryCode,
                           const std::string& entryId,
                           const std::wstring& text,
                           bool learningEnabled = true,
                           const std::string& layoutId = "",
                           const std::string& matchedCode = "",
                           const std::string& resolvedCode = "");
    bool RemovePendingCommit(const std::string& eventId);
    bool LoadAllPendingCommits(std::vector<EntryRepository::PendingCommitRecord>& outRecords);
    std::vector<EntryRepository::PendingCommitRecord> LoadAllPendingCommits();

    // User Dictionary Management
    std::string AddUserEntry(const std::wstring& text,
                            const std::vector<std::pair<InputScheme, std::string>>& codes);
    bool EditUserEntry(const std::string& entryId,
                       const std::wstring& newText,
                       const std::vector<std::pair<InputScheme, std::string>>& newCodes = {});
    bool SetUserEntryCodes(const std::string& entryId,
                           const std::vector<std::pair<InputScheme, std::string>>& newCodes);
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
    EntryRepository* GetRepo() { return m_repo.get(); }
    InputCoordinator* GetCoordinator() { return m_coordinator.get(); }
    SqliteDb& GetDb() { return m_db; }

    bool SavePolicy(const InputPolicy& policy,
                    std::optional<uint64_t> expectedRevision = std::nullopt,
                    std::string* errorOut = nullptr,
                    uint64_t* outRevision = nullptr,
                    bool* outConflict = nullptr);
    std::shared_ptr<const InputPolicy> GetPolicy() const;
    bool ResetAllStats();
    int64_t GetLastResetStatsAt();

private:
    mutable std::mutex m_policySaveMutex;
    std::shared_ptr<IClock> m_clock;
    SqliteDb m_db;
    std::unique_ptr<EntryRepository> m_repo;
    PinyinSegmenter m_pinyinSegmenter;
    BuiltinDictionary m_builtinDict;
    std::unique_ptr<Ranker> m_ranker;
    std::unique_ptr<InputCoordinator> m_coordinator;
    bool m_initialized = false;
};

} // namespace ime

