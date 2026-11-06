#include "json_backup.h"
#include "../dictionary/builtin_dict.h"
#include "../../third_party/nlohmann/json.hpp"
#include <fstream>
#include <chrono>
#include <cmath>
#include <unordered_set>

using json = nlohmann::json;

namespace ime {

bool JsonBackup::ExportToFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, std::string* errorOut) {
    try {
        SqliteDb::Transaction snapshot(db);
        if (!snapshot.IsStarted()) throw std::runtime_error("Failed to start backup snapshot");
        json root;
        root["version"] = "1.0";
        root["app"] = "PersonalIME";
        root["exported_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // 1. All entries (including user entries and modified builtin entries)
        auto entries = repo.GetAllEntries();
        json entriesJson = json::array();

        for (const auto& e : entries) {
            // A builtin skeleton left after resetting habits contains no user data.
            if (e.source == EntrySource::Builtin && e.codes.empty()) continue;
            if (!IsValidText(e.text) || e.codes.empty()) throw std::runtime_error("Invalid stored entry in backup: " + e.id);
            json item;
            item["id"] = e.id;
            item["text"] = WideToUtf8(e.text);

            json codesArr = json::array();
            json pinnedArr = json::array();
            json statsArr = json::array();

            for (const auto& [sch, code] : e.codes) {
                codesArr.push_back({
                    {"scheme", SchemeToString(sch)},
                    {"code", code}
                });

                auto pref = repo.GetSortPreference(sch, code, e.id);
                if (pref && pref->is_pinned) {
                    pinnedArr.push_back({
                        {"scheme", SchemeToString(sch)},
                        {"code", code},
                        {"rank", pref->order_rank}
                    });
                }

                auto stat = repo.GetStat(sch, code, e.id);
                if (stat && stat->selected_count > 0) {
                    statsArr.push_back({
                        {"scheme", SchemeToString(sch)},
                        {"code", code},
                        {"count", stat->selected_count},
                        {"last_used", stat->last_selected_at},
                        {"effective_count", stat->effective_count},
                        {"last_decay_at", stat->last_decay_at}
                    });
                }
            }

            item["codes"] = codesArr;
            item["pinned"] = pinnedArr;
            item["stats"] = statsArr;
            entriesJson.push_back(item);
        }

        root["entries"] = entriesJson;

        // 2. Global preferences (covers builtin words and prefix pins)
        auto allPrefs = repo.GetAllSortPreferences();
        json prefsJson = json::array();
        for (const auto& p : allPrefs) {
            prefsJson.push_back({
                {"scheme", SchemeToString(p.scheme)},
                {"code", p.code},
                {"entry_id", p.entry_id},
                {"is_pinned", p.is_pinned},
                {"rank", p.order_rank}
            });
        }
        root["preferences"] = prefsJson;

        // 3. Global stats (covers builtin words and prefix stats)
        auto allStats = repo.GetAllEntryStats();
        json statsJson = json::array();
        for (const auto& s : allStats) {
            statsJson.push_back({
                {"scheme", SchemeToString(s.scheme)},
                {"code", s.code},
                {"entry_id", s.entry_id},
                {"count", s.selected_count},
                {"last_used", s.last_selected_at},
                {"effective_count", s.effective_count},
                {"last_decay_at", s.last_decay_at}
            });
        }
        root["stats"] = statsJson;
        if (!snapshot.Commit()) throw std::runtime_error("Failed to complete backup snapshot");

        std::ofstream f(filePath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            if (errorOut) *errorOut = "Failed to open export file for writing: " + filePath;
            return false;
        }
        f << root.dump(2);
        f.flush();
        if (!f) throw std::runtime_error("Failed while writing backup file");
        return true;
    } catch (const std::exception& ex) {
        if (errorOut) *errorOut = std::string("Export error: ") + ex.what();
        return false;
    }
}

bool JsonBackup::ImportFromFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, bool replaceMode, std::string* errorOut, const BuiltinDictionary* builtinDict) {
    std::ifstream f(filePath);
    if (!f.is_open()) {
        if (errorOut) *errorOut = "Cannot open import file: " + filePath;
        return false;
    }

    json root;
    try {
        f >> root;
    } catch (const std::exception& ex) {
        if (errorOut) *errorOut = std::string("Invalid JSON format: ") + ex.what();
        return false;
    }

    if (!root.contains("version") || root["version"] != "1.0") {
        if (errorOut) *errorOut = "Unsupported or missing schema version in backup file (expected '1.0')";
        return false;
    }

    if (!root.contains("entries") || !root["entries"].is_array()) {
        if (errorOut) *errorOut = "Missing 'entries' array in backup file";
        return false;
    }

    if (root.contains("preferences") && !root["preferences"].is_array()) {
        if (errorOut) *errorOut = "Field 'preferences' must be an array";
        return false;
    }

    if (root.contains("stats") && !root["stats"].is_array()) {
        if (errorOut) *errorOut = "Field 'stats' must be an array";
        return false;
    }

    // ==========================================
    // Phase 1: STRICT PRE-VALIDATION
    // Ensure entire JSON file is valid BEFORE touching database!
    // ==========================================
    struct ParsedEntry {
        std::string id;
        std::wstring text;
        std::vector<std::pair<InputScheme, std::string>> codes;
        std::vector<std::tuple<InputScheme, std::string, int>> pinned;
        std::vector<std::tuple<InputScheme, std::string, int, int64_t, double, int64_t>> stats;
    };

    std::vector<ParsedEntry> validatedEntries;
    std::unordered_set<std::string> knownEntryIds;
    std::lock_guard<std::recursive_mutex> importLock(db.GetMutex());

    try {
        // 1. Validate entries
        for (const auto& item : root["entries"]) {
            if (!item.is_object()) {
                throw std::runtime_error("Entry item must be an object");
            }
            if (!item.contains("text") || !item["text"].is_string()) {
                throw std::runtime_error("Entry missing valid text string");
            }
            std::string textStr = item["text"].get<std::string>();
            std::wstring text = Utf8ToWide(textStr);
            if (!IsValidText(text)) {
                throw std::runtime_error("Entry text is empty or exceeds maximum length");
            }

            std::string id = item.value("id", "");
            if (id.rfind("b_", 0) == 0 && builtinDict && builtinDict->IsLoaded() && !builtinDict->HasItem(id)) {
                throw std::runtime_error("Unknown builtin entry id: " + id);
            }
            if (!id.empty()) {
                if (knownEntryIds.count(id) > 0) {
                    throw std::runtime_error("Duplicate entry id in backup: " + id);
                }
                knownEntryIds.insert(id);
            }

            if (!item.contains("codes") || !item["codes"].is_array()) {
                throw std::runtime_error("Entry missing codes array");
            }

            std::vector<std::pair<InputScheme, std::string>> codes;
            for (const auto& cItem : item["codes"]) {
                if (!cItem.is_object() || !cItem.contains("scheme") || !cItem.contains("code")) {
                    throw std::runtime_error("Code mapping missing scheme or code");
                }
                std::string schStr = cItem["scheme"].get<std::string>();
                std::string codeStr = cItem["code"].get<std::string>();
                InputScheme sch = SchemeFromString(schStr);
                if (sch == InputScheme::Unknown) {
                    throw std::runtime_error("Invalid or unknown scheme: " + schStr);
                }
                std::string lowerCode = codeStr;
                std::transform(lowerCode.begin(), lowerCode.end(), lowerCode.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!IsValidCode(sch, lowerCode)) {
                    throw std::runtime_error("Invalid code '" + codeStr + "' for scheme '" + schStr + "'");
                }
                codes.emplace_back(sch, lowerCode);
            }

            if (codes.empty()) {
                throw std::runtime_error("Entry has no valid codes");
            }

            std::vector<std::tuple<InputScheme, std::string, int>> pinned;
            if (item.contains("pinned") && !item["pinned"].is_array()) throw std::runtime_error("Entry pinned must be an array");
            if (item.contains("pinned") && item["pinned"].is_array()) {
                for (const auto& pItem : item["pinned"]) {
                    if (!pItem.is_object() || !pItem.contains("scheme") || !pItem.contains("code")) {
                        throw std::runtime_error("Entry pinned item must be an object with scheme and code");
                    }
                    InputScheme sch = SchemeFromString(pItem["scheme"].get<std::string>());
                    if (sch == InputScheme::Unknown) {
                        throw std::runtime_error("Invalid scheme in entry pinned preference");
                    }
                    std::string code = pItem["code"].get<std::string>();
                    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (!IsValidCode(sch, code)) {
                        throw std::runtime_error("Invalid code in entry pinned preference: " + code);
                    }
                    int rank = pItem.value("rank", 1);
                    if (rank < 1 || rank > 1000000) {
                        throw std::runtime_error("Rank out of bounds in entry pinned preference");
                    }
                    pinned.emplace_back(sch, code, rank);
                }
            }

            std::vector<std::tuple<InputScheme, std::string, int, int64_t, double, int64_t>> stats;
            if (item.contains("stats") && !item["stats"].is_array()) throw std::runtime_error("Entry stats must be an array");
            if (item.contains("stats") && item["stats"].is_array()) {
                for (const auto& sItem : item["stats"]) {
                    if (!sItem.is_object() || !sItem.contains("scheme") || !sItem.contains("code") || !sItem.contains("count")) {
                        throw std::runtime_error("Entry stat must contain scheme, code, and count");
                    }
                    InputScheme sch = SchemeFromString(sItem["scheme"].get<std::string>());
                    if (sch == InputScheme::Unknown) {
                        throw std::runtime_error("Invalid scheme in entry stat");
                    }
                    std::string code = sItem["code"].get<std::string>();
                    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (!IsValidCode(sch, code)) {
                        throw std::runtime_error("Invalid code in entry stat: " + code);
                    }
                    int count = sItem["count"].get<int>();
                    if (count < 0 || count > 1000000000) {
                        throw std::runtime_error("Selected count out of bounds in entry stat");
                    }
                    int64_t lastUsed = sItem.value("last_used", 0LL);
                    if (lastUsed < 0 || lastUsed > 253402300799LL) {
                        throw std::runtime_error("last_used timestamp out of bounds in entry stat");
                    }
                    double effCount = sItem.value("effective_count", static_cast<double>(count));
                    if (!std::isfinite(effCount) || effCount < 0.0 || effCount > 1000000000.0) {
                        throw std::runtime_error("effective_count invalid or out of bounds in entry stat");
                    }
                    int64_t lastDecayAt = sItem.value("last_decay_at", lastUsed);
                    if (lastDecayAt < 0 || lastDecayAt > 253402300799LL) {
                        throw std::runtime_error("last_decay_at timestamp out of bounds in entry stat");
                    }
                    stats.emplace_back(sch, code, count, lastUsed, effCount, lastDecayAt);
                }
            }

            validatedEntries.push_back({id, text, std::move(codes), std::move(pinned), std::move(stats)});
        }

        // If not replace mode, existing entries in database are also recognized
        if (!replaceMode) {
            for (const auto& e : repo.GetAllEntries()) {
                knownEntryIds.insert(e.id);
            }
        }

        auto isEntryIdValid = [&](const std::string& eid) -> bool {
            if (eid.empty()) return false;
            if (knownEntryIds.count(eid) > 0) return true;
            if (eid.rfind("b_", 0) == 0) {
                if (builtinDict && builtinDict->IsLoaded()) {
                    return builtinDict->HasItem(eid);
                }
                // When builtinDict is not loaded, validate builtin id structure
                if (eid.rfind("b_py_", 0) == 0 || eid.rfind("b_wb_", 0) == 0) {
                    return true;
                }
            }
            return false;
        };

        // 2. Validate global preferences
        if (root.contains("preferences") && root["preferences"].is_array()) {
            for (const auto& pItem : root["preferences"]) {
                if (!pItem.is_object() || !pItem.contains("scheme") || !pItem.contains("code") || !pItem.contains("entry_id")) {
                    throw std::runtime_error("Preference item missing scheme, code, or entry_id");
                }
                InputScheme sch = SchemeFromString(pItem["scheme"].get<std::string>());
                if (sch == InputScheme::Unknown) {
                    throw std::runtime_error("Unknown scheme in global preference");
                }
                std::string code = pItem["code"].get<std::string>();
                std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!IsValidCode(sch, code)) {
                    throw std::runtime_error("Invalid code in global preference: " + code);
                }
                std::string entryId = pItem["entry_id"].get<std::string>();
                if (!isEntryIdValid(entryId)) {
                    throw std::runtime_error("Global preference references dangling or unknown entry_id: " + entryId);
                }
                int rank = pItem.value("rank", 1);
                if (rank < 1 || rank > 1000000) {
                    throw std::runtime_error("Rank out of bounds in global preference");
                }
            }
        }

        // 3. Validate global stats
        if (root.contains("stats") && root["stats"].is_array()) {
            for (const auto& sItem : root["stats"]) {
                if (!sItem.is_object() || !sItem.contains("scheme") || !sItem.contains("code") || !sItem.contains("entry_id")) {
                    throw std::runtime_error("Stat item missing scheme, code, or entry_id");
                }
                InputScheme sch = SchemeFromString(sItem["scheme"].get<std::string>());
                if (sch == InputScheme::Unknown) {
                    throw std::runtime_error("Unknown scheme in global stat");
                }
                std::string code = sItem["code"].get<std::string>();
                std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!IsValidCode(sch, code)) {
                    throw std::runtime_error("Invalid code in global stat: " + code);
                }
                std::string entryId = sItem["entry_id"].get<std::string>();
                if (!isEntryIdValid(entryId)) {
                    throw std::runtime_error("Global stat references dangling or unknown entry_id: " + entryId);
                }
                int count = sItem.value("count", 0);
                if (count < 0 || count > 1000000000) {
                    throw std::runtime_error("Count out of bounds in global stat");
                }
                int64_t lastUsed = sItem.value("last_used", 0LL);
                if (lastUsed < 0 || lastUsed > 253402300799LL) {
                    throw std::runtime_error("last_used timestamp out of bounds in global stat");
                }
                double effCount = sItem.value("effective_count", static_cast<double>(count));
                if (!std::isfinite(effCount) || effCount < 0.0 || effCount > 1000000000.0) {
                    throw std::runtime_error("effective_count invalid or out of bounds in global stat");
                }
                int64_t lastDecayAt = sItem.value("last_decay_at", lastUsed);
                if (lastDecayAt < 0 || lastDecayAt > 253402300799LL) {
                    throw std::runtime_error("last_decay_at timestamp out of bounds in global stat");
                }
            }
        }

    } catch (const std::exception& ex) {
        if (errorOut) *errorOut = std::string("Validation failed: ") + ex.what();
        return false;
    }

    // ==========================================
    // Phase 2: DATABASE PERSISTENCE IN TRANSACTION
    // ==========================================
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) {
        if (errorOut) *errorOut = "Failed to start import transaction";
        return false;
    }

    try {
        if (replaceMode) {
            if (repo.HasAnyPendingCommits()) {
                throw std::runtime_error("Cannot replace database while unconfirmed pending commits are in flight");
            }
            // Clear all user entries, preferences, and stats (terminal commit_events audit log MUST be preserved)
            if (!db.Execute("DELETE FROM sort_preferences;") ||
                !db.Execute("DELETE FROM entry_stats;") ||
                !db.Execute("DELETE FROM entry_resets;") ||
                !db.Execute("DELETE FROM entries WHERE source = 1;") ||
                !db.Execute("DELETE FROM entry_codes WHERE entry_id NOT IN (SELECT id FROM entries);") ||
                !db.Execute("DELETE FROM entries WHERE source = 0;")) {
                throw std::runtime_error("Failed to clear database state in replace mode");
            }
        }

        for (const auto& entry : validatedEntries) {
            std::string actualId;
            if (entry.id.rfind("b_", 0) == 0) {
                if (!repo.EnsureBuiltinEntryExists(entry.id, entry.text)) {
                    throw std::runtime_error("Failed to ensure builtin entry: " + entry.id);
                }
                actualId = entry.id;
            } else if (!entry.id.empty()) {
                auto existing = repo.GetEntry(entry.id);
                if (existing) {
                    if (!repo.UpdateEntryText(entry.id, entry.text)) {
                        throw std::runtime_error("Failed to update entry text: " + entry.id);
                    }
                    if (!repo.AddEntryCodes(entry.id, entry.codes)) {
                        throw std::runtime_error("Failed to add entry codes: " + entry.id);
                    }
                    actualId = entry.id;
                }
            }

            if (actualId.empty()) {
                actualId = repo.AddEntry(entry.text, entry.codes, EntrySource::User, entry.id);
                if (actualId.empty()) {
                    throw std::runtime_error("Failed to add entry: " + WideToUtf8(entry.text));
                }
            }

            // Restore entry-level pinned preferences
            for (const auto& [sch, code, rank] : entry.pinned) {
                if (!repo.SetPinPreference(sch, code, actualId, true, rank)) {
                    throw std::runtime_error("Failed to set pin preference: " + actualId);
                }
            }

            // Restore entry-level stats with high fidelity (no looping)
            for (const auto& [sch, code, count, lastUsed, effCount, lastDecayAt] : entry.stats) {
                if (!repo.SetStatDirect(sch, code, actualId, count, lastUsed, effCount, lastDecayAt, !replaceMode)) {
                    throw std::runtime_error("Failed to set stat: " + actualId);
                }
            }
        }

        // Restore global preferences (including builtin words)
        if (root.contains("preferences") && root["preferences"].is_array()) {
            for (const auto& pItem : root["preferences"]) {
                if (pItem.contains("scheme") && pItem.contains("code") && pItem.contains("entry_id")) {
                    InputScheme sch = SchemeFromString(pItem["scheme"].get<std::string>());
                    if (sch != InputScheme::Unknown) {
                        std::string code = pItem["code"].get<std::string>();
                        std::string entryId = pItem["entry_id"].get<std::string>();
                        bool isPinned = pItem.value("is_pinned", true);
                        int rank = pItem.value("rank", 1);
                        if (entryId.rfind("b_", 0) == 0) {
                            auto candidate = builtinDict ? builtinDict->GetCandidateById(sch, code, entryId) : std::nullopt;
                            auto existing = repo.GetEntry(entryId);
                            if (!candidate && (!existing || existing->text.empty())) {
                                throw std::runtime_error("Builtin preference needs dictionary text: " + entryId);
                            }
                            if (!repo.EnsureBuiltinEntryExists(entryId, candidate ? candidate->text : existing->text)) {
                                throw std::runtime_error("Failed to ensure builtin entry for preference: " + entryId);
                            }
                        }
                        if (!repo.SetPinPreference(sch, code, entryId, isPinned, rank)) {
                            throw std::runtime_error("Failed to set preference: " + entryId);
                        }
                    }
                }
            }
        }

        // Restore global stats (including builtin words)
        if (root.contains("stats") && root["stats"].is_array()) {
            for (const auto& sItem : root["stats"]) {
                if (sItem.contains("scheme") && sItem.contains("code") && sItem.contains("entry_id")) {
                    InputScheme sch = SchemeFromString(sItem["scheme"].get<std::string>());
                    if (sch != InputScheme::Unknown) {
                        std::string code = sItem["code"].get<std::string>();
                        std::string entryId = sItem["entry_id"].get<std::string>();
                        int count = sItem.value("count", 0);
                        int64_t lastUsed = sItem.value("last_used", 0);
                        double effCount = sItem.value("effective_count", static_cast<double>(count));
                        int64_t lastDecayAt = sItem.value("last_decay_at", lastUsed);
                        if (entryId.rfind("b_", 0) == 0) {
                            auto candidate = builtinDict ? builtinDict->GetCandidateById(sch, code, entryId) : std::nullopt;
                            auto existing = repo.GetEntry(entryId);
                            if (!candidate && (!existing || existing->text.empty())) {
                                throw std::runtime_error("Builtin stat needs dictionary text: " + entryId);
                            }
                            if (!repo.EnsureBuiltinEntryExists(entryId, candidate ? candidate->text : existing->text)) {
                                throw std::runtime_error("Failed to ensure builtin entry for stat: " + entryId);
                            }
                        }
                        if (!repo.SetStatDirect(sch, code, entryId, count, lastUsed, effCount, lastDecayAt, !replaceMode)) {
                            throw std::runtime_error("Failed to set stat: " + entryId);
                        }
                    }
                }
            }
        }

        if (!tx.Commit()) {
            throw std::runtime_error("Transaction commit failed");
        }
        return true;
    } catch (const std::exception& ex) {
        tx.Rollback();
        if (errorOut) *errorOut = std::string("Import error: ") + ex.what();
        return false;
    }
}

} // namespace ime

