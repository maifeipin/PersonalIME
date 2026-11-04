#pragma once

// Included after the test runner's TEST_ASSERT definition.
static int TestStage2Finalization() {
    using json = nlohmann::json;
    auto save = [](const char* path, const json& j) {
        std::ofstream f(path); f << j.dump(2);
    };
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Initialize edit test");
        const auto original = L"符号 √ → 😀\n第二行";
        auto id = e.AddUserEntry(original, {{InputScheme::Custom, ";before"}});
        TEST_ASSERT(!id.empty(), "Unicode special phrase accepted");
        TEST_ASSERT(!e.EditUserEntry(id, L"after", {{InputScheme::Wubi86, "invalid-code"}}), "Reject invalid replacement");
        TEST_ASSERT(!e.SetUserEntryCodes(id, {{InputScheme::Custom, ";valid"}, {InputScheme::Wubi86, "zzzzz"}}), "Reject mixed invalid codes atomically");
        auto entry = e.GetUserEntry(id);
        TEST_ASSERT(entry && entry->text == original && entry->codes.size() == 1 && entry->codes[0].second == ";before", "Invalid edits preserve original state");
        TEST_ASSERT(!e.EditUserEntry(id, std::wstring(513, L'x')), "Edit enforces text limit");
        TEST_ASSERT(e.AddUserEntry(std::wstring(1, wchar_t(0xd800)), {{InputScheme::Custom, ";invalid"}}).empty(), "Reject isolated UTF16 surrogate");
        TEST_ASSERT(e.ExportData("test_final_unicode.json"), "Export Unicode phrase");
        ImeEngine restored;
        TEST_ASSERT(restored.Initialize(":memory:", "") && restored.ImportData("test_final_unicode.json", true), "Restore Unicode phrase");
        TEST_ASSERT(restored.GetUserEntry(id)->text == original, "Preserve symbols, Emoji and newline exactly");
        std::filesystem::remove("test_final_unicode.json");
    }
    {
        SqliteDb db;
        TEST_ASSERT(db.Open(":memory:") && Migration::Migrate(db), "Initialize transaction fault test");
        EntryRepository repo(db, std::make_shared<MockClock>());
        auto id = repo.AddEntry(L"keep", {{InputScheme::Custom, ";keep"}});
        auto denyBegin = [](void*, int op, const char* arg, const char*, const char*, const char*) {
            return op == SQLITE_TRANSACTION && std::string(arg) == "BEGIN" ? SQLITE_DENY : SQLITE_OK;
        };
        sqlite3_set_authorizer(db.GetRawHandle(), denyBegin, nullptr);
        save("test_final_begin.json", {{"version", "1.0"}, {"entries", json::array()}});
        TEST_ASSERT(!JsonBackup::ImportFromFile(repo, db, "test_final_begin.json", true), "Refuse import when BEGIN fails");
        TEST_ASSERT(repo.GetEntry(id).has_value(), "Failed BEGIN must not clear old data");
        TEST_ASSERT(repo.AddEntry(L"blocked", {{InputScheme::Custom, ";blocked"}}).empty(), "Failed BEGIN prevents entry insertion");
        TEST_ASSERT(repo.QueryUserCandidates(InputScheme::Custom, ";blocked").empty(), "No autocommitted partial entry");
        sqlite3_set_authorizer(db.GetRawHandle(), nullptr, nullptr);
        TEST_ASSERT(repo.UpdateEntryText(id, L"kept"), "Connection remains usable after failed BEGIN");
        auto denyEntriesRead = [](void*, int op, const char* table, const char*, const char*, const char*) {
            return op == SQLITE_READ && std::string(table) == "entries" ? SQLITE_DENY : SQLITE_OK;
        };
        save("test_final_export.json", {{"sentinel", true}});
        sqlite3_set_authorizer(db.GetRawHandle(), denyEntriesRead, nullptr);
        TEST_ASSERT(!JsonBackup::ExportToFile(repo, db, "test_final_export.json"), "Read failure cannot export a successful empty backup");
        sqlite3_set_authorizer(db.GetRawHandle(), nullptr, nullptr);
        std::ifstream saved("test_final_export.json"); json previous; saved >> previous; saved.close();
        TEST_ASSERT(previous.value("sentinel", false), "Failed backup read preserves previous output file");
        std::filesystem::remove("test_final_export.json");
        std::filesystem::remove("test_final_begin.json");
    }
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", "data/dictionaries"), "Initialize separated Pinyin test");
        std::string id;
        for (const auto& c : e.Query(InputScheme::Pinyin, "xi'an").candidates) if (c.text == L"西安") id = c.entry_id;
        TEST_ASSERT(!id.empty(), "Separated query returns 西安");
        TEST_ASSERT(e.CommitCandidate("final-separated", InputScheme::Pinyin, "xi'an", id), "Visible separated candidate can learn");
        TEST_ASSERT(e.CommitCandidate("final-separated", InputScheme::Pinyin, "xi'an", id), "Separated retry is idempotent");
        TEST_ASSERT(e.PinCandidate(InputScheme::Pinyin, "xi'an", id), "Separated candidate can be pinned");
        auto q = e.Query(InputScheme::Pinyin, "xi'an");
        TEST_ASSERT(q.candidates[0].entry_id == id && q.candidates[0].is_pinned && q.candidates[0].selected_count == 1, "Separated recall keeps pin and one learning event");
        save("test_final_metadata.json", {{"version", "1.0"}, {"entries", json::array()},
            {"preferences", json::array({{{"scheme", "pinyin"}, {"code", "xi'an"}, {"entry_id", id}, {"rank", 1}}})}});
        TEST_ASSERT(e.ImportData("test_final_metadata.json", true), "Import dictionary-backed metadata without duplicate entry text");
        TEST_ASSERT(e.ExportData("test_final_reexport.json"), "Re-export metadata-only state");
        ImeEngine restored;
        TEST_ASSERT(restored.Initialize(":memory:", "data/dictionaries"), "Initialize metadata restore");
        std::string error;
        TEST_ASSERT(restored.ImportData("test_final_reexport.json", true, &error), "Metadata-only state has a valid roundtrip");
        TEST_ASSERT(restored.Query(InputScheme::Pinyin, "xi'an").candidates[0].is_pinned, "Restored separated pin survives");
        TEST_ASSERT(restored.UnpinCandidate(InputScheme::Pinyin, "xi'an", id) && restored.ResetStats(), "Reset builtin personal state");
        TEST_ASSERT(restored.ExportData("test_final_reexport.json"), "Reset builtin skeleton can still export");
        TEST_ASSERT(e.ImportData("test_final_reexport.json", true), "Backup after reset is importable");
        std::filesystem::remove("test_final_metadata.json");
        std::filesystem::remove("test_final_reexport.json");
        TEST_ASSERT(!e.Initialize(":memory:", "missing-finalization-dictionaries"), "Failed reinitialization reports failure");
        TEST_ASSERT(e.Query(InputScheme::Pinyin, "xi'an").candidates.empty(), "Failed reinitialization cannot expose stale dictionary");
    }
    {
        SqliteDb db;
        TEST_ASSERT(db.Open(":memory:") && Migration::Migrate(db), "Prepare real v1 migration fixture");
        TEST_ASSERT(db.Execute("DROP TABLE commit_events; CREATE TABLE commit_events(event_id TEXT PRIMARY KEY, committed_at INTEGER NOT NULL); INSERT INTO commit_events VALUES('legacy',1700000000); PRAGMA user_version=1;"), "Create old event and version");
        auto denyVersion = [](void*, int op, const char* key, const char* value, const char*, const char*) {
            return op == SQLITE_PRAGMA && std::string(key) == "user_version" && value && std::string(value) == "2" ? SQLITE_DENY : SQLITE_OK;
        };
        sqlite3_set_authorizer(db.GetRawHandle(), denyVersion, nullptr);
        TEST_ASSERT(!Migration::Migrate(db), "Version-write failure aborts upgrade");
        sqlite3_set_authorizer(db.GetRawHandle(), nullptr, nullptr);
        TEST_ASSERT(Migration::GetSchemaVersion(db) == 1, "Failed upgrade retains v1 version");
        auto columns = db.Prepare("PRAGMA table_info(commit_events);"); int count = 0; while (columns->Step()) ++count;
        TEST_ASSERT(count == 2, "Failed upgrade rolls back all added columns");
        TEST_ASSERT(Migration::Migrate(db) && Migration::GetSchemaVersion(db) == Migration::CURRENT_SCHEMA_VERSION, "Retry upgrades v1 successfully");
        EntryRepository repo(db, std::make_shared<MockClock>());
        auto id = repo.AddEntry(L"migration", {{InputScheme::Custom, ";migration"}});
        TEST_ASSERT(repo.RecordCommit("legacy", InputScheme::Custom, ";migration", id), "Legacy event remains idempotent");
        TEST_ASSERT(!repo.GetStat(InputScheme::Custom, ";migration", id), "Legacy retry cannot increment stats");
        TEST_ASSERT(repo.RecordCommit("new", InputScheme::Custom, ";migration", id), "New event learns after migration");
        TEST_ASSERT(repo.GetStat(InputScheme::Custom, ";migration", id)->selected_count == 1, "New learning counted exactly once");
    }
    std::cout << "[PASS] Stage 2 finalization: invalid edits, failed BEGIN, Unicode, separated learning and metadata roundtrip\n";
    return 0;
}
