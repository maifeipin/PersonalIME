#include "ime_engine.h"
#include "../storage/migration.h"
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

    if (!dictDir.empty()) {
        if (!m_builtinDict.Load(dictDir)) {
            return false;
        }
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

    return m_repo->RecordCommit(eventId, scheme, clean, entryId);
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

} // namespace ime
