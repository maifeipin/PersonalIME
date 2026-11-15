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
#include "../storage/plugin_repository.h"
#include "../storage/emoji_repository.h"
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
    bool InitializeCore(const std::string& dbPath,
                        std::shared_ptr<IClock> clock = nullptr);
    bool LoadDictionaries(const std::string& dictDir);
    bool IsCoreInitialized() const { return m_coreInitialized; }
    bool IsInitialized() const { return m_initialized; }

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
                           const std::string& resolvedCode = "",
                           uint64_t contextEpoch = 0,
                           uint64_t inputGeneration = 0,
                           uint64_t ownerInstanceId = 0);
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
    std::shared_ptr<const BuiltinDictionary> GetBuiltinDictSnapshot() const;
    EntryRepository* GetRepo() { return m_repo.get(); }
    PluginRepository* GetPluginRepo() { return m_pluginRepo.get(); }
    EmojiRepository* GetEmojiRepo() { return m_emojiRepo.get(); }
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
    mutable std::mutex m_dictSnapshotMutex;
    std::shared_ptr<IClock> m_clock;
    SqliteDb m_db;
    std::unique_ptr<EntryRepository> m_repo;
    std::unique_ptr<PluginRepository> m_pluginRepo;
    std::unique_ptr<EmojiRepository> m_emojiRepo;
    PinyinSegmenter m_pinyinSegmenter;
    std::shared_ptr<const BuiltinDictionary> m_initialEmptyDict;
    std::shared_ptr<const BuiltinDictionary> m_builtinDictSnapshot;
    std::unique_ptr<Ranker> m_ranker;
    std::unique_ptr<InputCoordinator> m_coordinator;
    bool m_coreInitialized = false;
    bool m_initialized = false;
};

} // namespace ime

