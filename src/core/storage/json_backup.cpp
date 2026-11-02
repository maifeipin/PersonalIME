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

        auto entries = repo.GetAllUserEntries();
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
                        {"last_used", stat->last_selected_at}
                    });
                }
            }

            item["codes"] = codesArr;
            item["pinned"] = pinnedArr;
            item["stats"] = statsArr;
            entriesJson.push_back(item);
        }

        root["entries"] = entriesJson;

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

    SqliteDb::Transaction tx(db);

    try {
        if (replaceMode) {
            // Clear all user entries and associated data
            db.Execute("DELETE FROM entries WHERE source = 1;");
            db.Execute("DELETE FROM entry_codes WHERE entry_id NOT IN (SELECT id FROM entries);");
            db.Execute("DELETE FROM sort_preferences WHERE entry_id NOT IN (SELECT id FROM entries);");
            db.Execute("DELETE FROM entry_stats WHERE entry_id NOT IN (SELECT id FROM entries);");
        }

        for (const auto& item : root["entries"]) {
            if (!item.contains("text") || !item["text"].is_string()) {
                throw std::runtime_error("Entry missing valid text string");
            }
            std::string textStr = item["text"].get<std::string>();
            if (textStr.empty()) {
                throw std::runtime_error("Entry text cannot be empty");
            }
            if (textStr.length() > 512) {
                throw std::runtime_error("Entry text exceeds maximum allowed length");
            }

            std::wstring text = Utf8ToWide(textStr);
            std::string id = item.value("id", "");

            std::vector<std::pair<InputScheme, std::string>> codes;
            if (item.contains("codes") && item["codes"].is_array()) {
                for (const auto& cItem : item["codes"]) {
                    if (cItem.contains("scheme") && cItem.contains("code")) {
                        std::string schStr = cItem["scheme"].get<std::string>();
                        std::string codeStr = cItem["code"].get<std::string>();
                        codes.emplace_back(SchemeFromString(schStr), codeStr);
                    }
                }
            }

            if (codes.empty()) {
                throw std::runtime_error("Entry has no valid codes");
            }

            // Check if entry already exists in merge mode
            std::string actualId;
            if (!id.empty()) {
                auto existing = repo.GetEntry(id);
                if (existing) {
                    repo.UpdateEntryText(id, text);
                    actualId = id;
                }
            }

            if (actualId.empty()) {
                actualId = repo.AddEntry(text, codes, EntrySource::User, id);
                if (actualId.empty()) {
                    throw std::runtime_error("Failed to add entry: " + textStr);
                }
            }

            // Restore pinned preferences
            if (item.contains("pinned") && item["pinned"].is_array()) {
                for (const auto& pItem : item["pinned"]) {
                    if (pItem.contains("scheme") && pItem.contains("code")) {
                        InputScheme sch = SchemeFromString(pItem["scheme"].get<std::string>());
                        std::string code = pItem["code"].get<std::string>();
                        int rank = pItem.value("rank", 1);
                        repo.SetPinPreference(sch, code, actualId, true, rank);
                    }
                }
            }

            // Restore stats
            if (item.contains("stats") && item["stats"].is_array()) {
                for (const auto& sItem : item["stats"]) {
                    if (sItem.contains("scheme") && sItem.contains("code") && sItem.contains("count")) {
                        InputScheme sch = SchemeFromString(sItem["scheme"].get<std::string>());
                        std::string code = sItem["code"].get<std::string>();
                        int count = sItem["count"].get<int>();
                        int64_t lastUsed = sItem.value("last_used", 0);
                        // Record multiple commits or direct upsert
                        for (int i = 0; i < count; ++i) {
                            repo.RecordCommit("", sch, code, actualId);
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

