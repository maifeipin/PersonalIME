#include "ime_engine.h"
#include "../storage/migration.h"
#include "../strategy/input_policy_json.h"
#include <algorithm>
#include <iostream>

namespace ime {

ImeEngine::ImeEngine() {}

ImeEngine::~ImeEngine() {}

bool ImeEngine::Initialize(const std::string& dbPath,
                           const std::string& dictDir,
                           std::shared_ptr<IClock> clock) {
    m_initialized = false;
    m_builtinDict = BuiltinDictionary();
    m_clock = clock ? clock : std::make_shared<SystemClock>();
    m_ranker = std::make_unique<Ranker>(m_clock);

    if (!m_db.Open(dbPath)) {
        return false;
    }

    if (!Migration::Migrate(m_db)) {
        return false;
    }

    m_repo = std::make_unique<EntryRepository>(m_db, m_clock);
    m_repo->SetBuiltinDictionary(&m_builtinDict);

    if (!dictDir.empty()) {
        if (!m_builtinDict.Load(dictDir)) {
            return false;
        }
    }

    m_coordinator = std::make_unique<InputCoordinator>(*m_repo, m_builtinDict, m_clock);

    // Load persisted input policy if present
    auto savedPolicyJson = m_repo->GetMetaSetting("input_policy");
    if (savedPolicyJson && !savedPolicyJson->empty()) {
        try {
            auto j = nlohmann::json::parse(*savedPolicyJson);
            InputPolicy policy;
            if (InputPolicyFromJson(j, policy)) {
                m_coordinator->SetPolicy(policy);
            }
        } catch (...) {}
    }

    m_initialized = true;
    return true;
}

QueryResult ImeEngine::Query(InputScheme scheme, const std::string& code) {
    QueryResult qr;
    qr.scheme = scheme;
    qr.query_code = code;
    if (!m_initialized || code.empty() || scheme == InputScheme::Unknown) return qr;

    std::string clean = code;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

    std::vector<Candidate> candidates;

    if (scheme == InputScheme::Wubi86) {
        auto builtinCands = m_builtinDict.QueryWubi(clean);
        auto userCands = m_repo->QueryUserCandidates(InputScheme::Wubi86, clean);

        candidates.insert(candidates.end(), std::make_move_iterator(builtinCands.begin()), std::make_move_iterator(builtinCands.end()));
        candidates.insert(candidates.end(), std::make_move_iterator(userCands.begin()), std::make_move_iterator(userCands.end()));
    } else if (scheme == InputScheme::Pinyin) {
        auto builtinCands = m_builtinDict.QueryPinyin(clean, m_pinyinSegmenter);
        auto userCands = m_repo->QueryUserCandidates(InputScheme::Pinyin, clean);

        candidates.insert(candidates.end(), std::make_move_iterator(builtinCands.begin()), std::make_move_iterator(builtinCands.end()));
        candidates.insert(candidates.end(), std::make_move_iterator(userCands.begin()), std::make_move_iterator(userCands.end()));
    } else if (scheme == InputScheme::Custom) {
        auto userCands = m_repo->QueryUserCandidates(InputScheme::Custom, clean);
        candidates.insert(candidates.end(), std::make_move_iterator(userCands.begin()), std::make_move_iterator(userCands.end()));
    }

    // Recall any pinned candidates or active stat candidates beyond the bounded pool
    std::unordered_set<std::string> seenIds;
    for (const auto& c : candidates) {
        seenIds.insert(c.entry_id);
    }

    auto pinnedPrefs = m_repo->GetPinnedPreferences(scheme, clean);
    for (const auto& pref : pinnedPrefs) {
        if (seenIds.count(pref.entry_id) == 0) {
            if (pref.entry_id.rfind("b_", 0) == 0) {
                auto cand = m_builtinDict.GetCandidateById(scheme, clean, pref.entry_id);
                if (cand.has_value()) {
                    seenIds.insert(pref.entry_id);
                    candidates.push_back(std::move(*cand));
                }
            } else {
                auto userEntry = m_repo->GetEntry(pref.entry_id);
                if (userEntry.has_value()) {
                    for (const auto& [entryScheme, entryCode] : userEntry->codes) {
                        if (entryScheme == scheme && entryCode.rfind(clean, 0) == 0) {
                            Candidate c;
                            c.entry_id = userEntry->id;
                            c.text = userEntry->text;
                            c.scheme = scheme;
                            c.matched_code = entryCode;
                            c.quality = (entryCode == clean) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;
                            c.base_weight = 50.0;
                            c.source = userEntry->source;
                            seenIds.insert(userEntry->id);
                            candidates.push_back(std::move(c));
                            break;
                        }
                    }
                }
            }
        }
    }

    auto activeStats = m_repo->GetActiveStats(scheme, clean);
    for (const auto& stat : activeStats) {
        if (seenIds.count(stat.entry_id) == 0) {
            if (stat.entry_id.rfind("b_", 0) == 0) {
                auto cand = m_builtinDict.GetCandidateById(scheme, clean, stat.entry_id);
                if (cand.has_value()) {
                    seenIds.insert(stat.entry_id);
                    candidates.push_back(std::move(*cand));
                }
            } else {
                auto userEntry = m_repo->GetEntry(stat.entry_id);
                if (userEntry.has_value()) {
                    for (const auto& [entryScheme, entryCode] : userEntry->codes) {
                        if (entryScheme == scheme && entryCode.rfind(clean, 0) == 0) {
                            Candidate c;
                            c.entry_id = userEntry->id;
                            c.text = userEntry->text;
                            c.scheme = scheme;
                            c.matched_code = entryCode;
                            c.quality = (entryCode == clean) ? MatchQuality::ExactFull : MatchQuality::PrefixCompletion;
                            c.base_weight = 50.0;
                            c.source = userEntry->source;
                            seenIds.insert(userEntry->id);
                            candidates.push_back(std::move(c));
                            break;
                        }
                    }
                }
            }
        }
    }

    // Attach user learning stats & pinned preference to builtin items if recorded
    for (auto& c : candidates) {
        if (c.source == EntrySource::Builtin) {
            auto pref = m_repo->GetSortPreference(scheme, clean, c.entry_id);
            if (pref) {
                c.is_pinned = pref->is_pinned;
                c.order_rank = pref->order_rank;
            }

            auto stat = m_repo->GetStat(scheme, clean, c.entry_id);
            if (stat) {
                c.selected_count = stat->selected_count;
                c.last_selected_at = stat->last_selected_at;
                // Calculate decayed effective count
                int64_t now = m_clock->Now();
                if (stat->last_decay_at > 0 && now > stat->last_decay_at && stat->effective_count > 0.0) {
                    double dt = static_cast<double>(now - stat->last_decay_at);
                    double factor = std::pow(2.0, -dt / (30.0 * 86400.0));
                    c.effective_count = stat->effective_count * factor;
                } else {
                    c.effective_count = stat->effective_count;
                }
            }
        }
    }

    qr.candidates = m_ranker->Rank(std::move(candidates));
    return qr;
}

bool ImeEngine::CommitCandidate(const std::string& eventId,
                                uint64_t sessionId,
                                uint64_t contextId,
                                InputScheme scheme,
                                const std::string& code,
                                const std::string& entryId) {
    if (!m_initialized || code.empty() || entryId.empty() || scheme == InputScheme::Unknown) return false;

    std::string clean = code;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

    // If this is a builtin entry, ensure an entry row exists in SQLite for foreign key integrity
    if (entryId.rfind("b_", 0) == 0) {
        auto builtinCand = m_builtinDict.GetCandidateById(scheme, clean, entryId);
        if (!builtinCand.has_value()) {
            return false;
        }
        if (!m_repo->EnsureBuiltinEntryExists(entryId, builtinCand->text)) return false;
    }

    return m_repo->RecordCommit(eventId, sessionId, contextId, scheme, clean, entryId);
}

bool ImeEngine::RecordCancel(const std::string& eventId, uint64_t sessionId, uint64_t contextId) {
    if (!m_initialized || !m_repo) return false;
    return m_repo->RecordCancel(eventId, sessionId, contextId);
}

EntryRepository::CommitEventRecord ImeEngine::GetCommitEventInfo(const std::string& eventId) {
    if (!m_initialized || !m_repo) return {};
    return m_repo->GetCommitEventRecord(eventId);
}

bool ImeEngine::IsCommitRecorded(const std::string& eventId) {
    if (!m_initialized || !m_repo) return false;
    return m_repo->HasCommitEvent(eventId);
}

bool ImeEngine::SavePendingCommit(const std::string& eventId,
                                  uint64_t sessionId,
                                  uint64_t contextId,
                                  InputScheme scheme,
                                  const std::string& queryCode,
                                  const std::string& entryId,
                                  const std::wstring& text,
                                  bool learningEnabled,
                                  const std::string& layoutId,
                                  const std::string& matchedCode,
                                  const std::string& resolvedCode) {
    if (!m_initialized || !m_repo || eventId.empty()) return false;
    EntryRepository::PendingCommitRecord rec;
    rec.event_id = eventId;
    rec.session_id = sessionId;
    rec.context_id = contextId;
    rec.scheme = scheme;
    rec.query_code = queryCode;
    rec.entry_id = entryId;
    rec.text = text;
    rec.created_at = m_clock ? m_clock->Now() : 0;
    rec.learning_enabled = learningEnabled;
    rec.layout_id = layoutId;
    rec.matched_code = matchedCode;
    rec.resolved_code = resolvedCode;
    return m_repo->SavePendingCommit(rec);
}

bool ImeEngine::RemovePendingCommit(const std::string& eventId) {
    if (!m_initialized || !m_repo) return false;
    return m_repo->RemovePendingCommit(eventId);
}

bool ImeEngine::LoadAllPendingCommits(std::vector<EntryRepository::PendingCommitRecord>& outRecords) {
    if (!m_initialized || !m_repo) return false;
    return m_repo->LoadAllPendingCommits(outRecords);
}

std::vector<EntryRepository::PendingCommitRecord> ImeEngine::LoadAllPendingCommits() {
    std::vector<EntryRepository::PendingCommitRecord> recs;
    LoadAllPendingCommits(recs);
    return recs;
}

std::string ImeEngine::AddUserEntry(const std::wstring& text,
                                    const std::vector<std::pair<InputScheme, std::string>>& codes) {
    if (!m_initialized) return "";
    return m_repo->AddEntry(text, codes);
}

bool ImeEngine::EditUserEntry(const std::string& entryId,
                              const std::wstring& newText,
                              const std::vector<std::pair<InputScheme, std::string>>& newCodes) {
    if (!m_initialized || !m_repo) return false;
    if (!newCodes.empty()) {
        return m_repo->UpdateEntry(entryId, newText, newCodes);
    }
    return m_repo->UpdateEntryText(entryId, newText);
}

bool ImeEngine::SetUserEntryCodes(const std::string& entryId,
                                  const std::vector<std::pair<InputScheme, std::string>>& newCodes) {
    if (!m_initialized || !m_repo) return false;
    return m_repo->SetEntryCodes(entryId, newCodes);
}

bool ImeEngine::DeleteUserEntry(const std::string& entryId) {
    if (!m_initialized) return false;
    return m_repo->DeleteEntry(entryId);
}

std::optional<Entry> ImeEngine::GetUserEntry(const std::string& entryId) {
    if (!m_initialized) return std::nullopt;
    return m_repo->GetEntry(entryId);
}

std::vector<Entry> ImeEngine::GetAllUserEntries() {
    if (!m_initialized) return {};
    return m_repo->GetAllUserEntries();
}

bool ImeEngine::PinCandidate(InputScheme scheme, const std::string& code, const std::string& entryId, int rank) {
    if (!m_initialized || scheme == InputScheme::Unknown || code.empty() || entryId.empty()) return false;
    std::string clean = code;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

    if (entryId.rfind("b_", 0) == 0) {
        auto builtinCand = m_builtinDict.GetCandidateById(scheme, clean, entryId);
        if (!builtinCand.has_value()) {
            return false;
        }
        if (!m_repo->EnsureBuiltinEntryExists(entryId, builtinCand->text)) return false;
    } else {
        if (!m_repo->VerifyCandidateAffiliation(scheme, clean, entryId)) {
            return false;
        }
    }

    return m_repo->SetPinPreference(scheme, clean, entryId, true, rank);
}

bool ImeEngine::UnpinCandidate(InputScheme scheme, const std::string& code, const std::string& entryId) {
    if (!m_initialized || scheme == InputScheme::Unknown || code.empty() || entryId.empty()) return false;
    std::string clean = code;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });
    return m_repo->UnpinPreference(scheme, clean, entryId);
}

bool ImeEngine::ResetStats(const std::string& entryId) {
    if (!m_initialized) return false;
    return m_repo->ResetStats(entryId);
}

bool ImeEngine::ExportData(const std::string& jsonFilePath, std::string* errorOut) {
    if (!m_initialized) return false;
    return JsonBackup::ExportToFile(*m_repo, m_db, jsonFilePath, errorOut);
}

bool ImeEngine::ImportData(const std::string& jsonFilePath, bool replaceMode, std::string* errorOut) {
    if (!m_initialized) return false;
    return JsonBackup::ImportFromFile(*m_repo, m_db, jsonFilePath, replaceMode, errorOut, &m_builtinDict);
}

std::vector<std::string> ImeEngine::ExplainCandidates(InputScheme scheme, const std::string& code) {
    std::vector<std::string> explanations;
    auto qr = Query(scheme, code);
    int idx = 1;
    for (const auto& c : qr.candidates) {
        std::string line = std::to_string(idx++) + ". " + WideToUtf8(c.text) + " (" + c.entry_id + ") " + c.explain_info;
        explanations.push_back(line);
    }
    return explanations;
}

bool ImeEngine::SavePolicy(const InputPolicy& policy,
                           std::optional<uint64_t> expectedRevision,
                           std::string* errorOut,
                           uint64_t* outRevision,
                           bool* outConflict) {
    if (outConflict) *outConflict = false;
    std::lock_guard<std::mutex> lock(m_policySaveMutex);
    if (!m_initialized || !m_coordinator || !m_repo) {
        if (errorOut) *errorOut = "Engine not initialized";
        return false;
    }
    if (!ValidateInputPolicy(policy, errorOut)) {
        return false;
    }

    auto currentPolicy = m_coordinator->GetPolicySnapshot();
    if (expectedRevision.has_value()) {
        uint64_t curRev = currentPolicy ? currentPolicy->revision : 0;
        if (curRev != *expectedRevision) {
            if (outConflict) *outConflict = true;
            if (errorOut) *errorOut = "Policy revision conflict: current revision is " + std::to_string(curRev);
            return false;
        }
    }

    InputPolicy toSave = policy;

    // Atomically bump revision from current live snapshot
    uint64_t nextRev = (currentPolicy ? currentPolicy->revision : 0) + 1;
    toSave.revision = nextRev;

    // Persist to meta_settings FIRST (fail-safe atomic update)
    try {
        auto j = InputPolicyToJson(toSave);
        if (!m_repo->SetMetaSetting("input_policy", j.dump())) {
            if (errorOut) *errorOut = "Failed to write input_policy to database";
            return false;
        }
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }

    // Only update in-memory coordinator if database write succeeded!
    m_coordinator->SetPolicy(toSave);
    if (outRevision) {
        *outRevision = toSave.revision;
    }
    return true;
}

std::shared_ptr<const InputPolicy> ImeEngine::GetPolicy() const {
    if (!m_coordinator) return nullptr;
    return m_coordinator->GetPolicySnapshot();
}

bool ImeEngine::ResetAllStats() {
    if (!m_repo) return false;
    return m_repo->ResetAllStats();
}

int64_t ImeEngine::GetLastResetStatsAt() {
    if (!m_repo) return 0;
    return m_repo->GetLastResetStatsAt();
}

} // namespace ime
