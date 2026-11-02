#include "json_backup.h"
#include "../../third_party/nlohmann/json.hpp"
#include <fstream>
#include <chrono>

using json = nlohmann::json;

namespace ime {

bool JsonBackup::ExportToFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, std::string* errorOut) {
    try {
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

        std::ofstream f(filePath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            if (errorOut) *errorOut = "Failed to open export file for writing: " + filePath;
            return false;
        }
        f << root.dump(2);
        return true;
    } catch (const std::exception& ex) {
        if (errorOut) *errorOut = std::string("Export error: ") + ex.what();
        return false;
    }
}

bool JsonBackup::ImportFromFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, bool replaceMode, std::string* errorOut) {
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

    try {
        for (const auto& item : root["entries"]) {
            if (!item.contains("text") || !item["text"].is_string()) {
                throw std::runtime_error("Entry missing valid text string");
            }
            std::string textStr = item["text"].get<std::string>();
            std::wstring text = Utf8ToWide(textStr);
            if (!IsValidText(text)) {
                throw std::runtime_error("Entry text is empty or exceeds maximum length");
            }

            std::string id = item.value("id", "");

            if (!item.contains("codes") || !item["codes"].is_array()) {
                throw std::runtime_error("Entry missing codes array");
            }

            std::vector<std::pair<InputScheme, std::string>> codes;
            for (const auto& cItem : item["codes"]) {
                if (!cItem.contains("scheme") || !cItem.contains("code")) {
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
            if (item.contains("pinned") && item["pinned"].is_array()) {
                for (const auto& pItem : item["pinned"]) {
                    if (pItem.contains("scheme") && pItem.contains("code")) {
                        InputScheme sch = SchemeFromString(pItem["scheme"].get<std::string>());
                        if (sch != InputScheme::Unknown) {
                            std::string code = pItem["code"].get<std::string>();
                            int rank = pItem.value("rank", 1);
                            pinned.emplace_back(sch, code, rank);
                        }
                    }
                }
            }

            std::vector<std::tuple<InputScheme, std::string, int, int64_t, double, int64_t>> stats;
            if (item.contains("stats") && item["stats"].is_array()) {
                for (const auto& sItem : item["stats"]) {
                    if (sItem.contains("scheme") && sItem.contains("code") && sItem.contains("count")) {
                        InputScheme sch = SchemeFromString(sItem["scheme"].get<std::string>());
                        if (sch != InputScheme::Unknown) {
                            std::string code = sItem["code"].get<std::string>();
                            int count = sItem["count"].get<int>();
                            int64_t lastUsed = sItem.value("last_used", 0);
                            double effCount = sItem.value("effective_count", static_cast<double>(count));
                            int64_t lastDecayAt = sItem.value("last_decay_at", lastUsed);
                            stats.emplace_back(sch, code, count, lastUsed, effCount, lastDecayAt);
                        }
                    }
                }
            }

            validatedEntries.push_back({id, text, std::move(codes), std::move(pinned), std::move(stats)});
        }
    } catch (const std::exception& ex) {
        if (errorOut) *errorOut = std::string("Validation failed: ") + ex.what();
        return false;
    }

    // ==========================================
    // Phase 2: DATABASE PERSISTENCE IN TRANSACTION
    // ==========================================
    SqliteDb::Transaction tx(db);

    try {
        if (replaceMode) {
            // Clear all user entries and associated data
            db.Execute("DELETE FROM entries WHERE source = 1;");
            db.Execute("DELETE FROM entry_codes WHERE entry_id NOT IN (SELECT id FROM entries);");
            db.Execute("DELETE FROM sort_preferences WHERE entry_id NOT IN (SELECT id FROM entries);");
            db.Execute("DELETE FROM entry_stats WHERE entry_id NOT IN (SELECT id FROM entries);");
        }

        for (const auto& entry : validatedEntries) {
            std::string actualId;
            if (entry.id.rfind("b_", 0) == 0) {
                repo.EnsureBuiltinEntryExists(entry.id, entry.text);
                actualId = entry.id;
            } else if (!entry.id.empty()) {
                auto existing = repo.GetEntry(entry.id);
                if (existing) {
                    repo.UpdateEntryText(entry.id, entry.text);
                    repo.AddEntryCodes(entry.id, entry.codes);
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
                repo.SetPinPreference(sch, code, actualId, true, rank);
            }

            // Restore entry-level stats with high fidelity (no looping)
            for (const auto& [sch, code, count, lastUsed, effCount, lastDecayAt] : entry.stats) {
                repo.SetStatDirect(sch, code, actualId, count, lastUsed, effCount, lastDecayAt, !replaceMode);
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
                            repo.EnsureBuiltinEntryExists(entryId, L"");
                        }
                        repo.SetPinPreference(sch, code, entryId, isPinned, rank);
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
                            repo.EnsureBuiltinEntryExists(entryId, L"");
                        }
                        repo.SetStatDirect(sch, code, entryId, count, lastUsed, effCount, lastDecayAt, !replaceMode);
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

