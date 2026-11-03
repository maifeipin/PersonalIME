#include <iostream>
#include <fstream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>
#include "../src/core/engine/ime_engine.h"
#include "../src/core/storage/migration.h"
#include "../src/core/clock.h"
#include "../src/core/types.h"
#include "../src/third_party/nlohmann/json.hpp"

using namespace ime;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[TEST FAILED] " << msg << " at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

// 1. Test Pinyin Ambiguity & Separators
int TestPinyinAmbiguityAndSeparators(ImeEngine& engine) {
    std::cout << "[TEST] Testing Pinyin Ambiguity & Separators (xian vs xi'an, fangan vs fang'an)..." << std::endl;

    // 1. Query continuous "xian": must match both single syllable "先" and multi-syllable "西安"
    auto qrXian = engine.Query(InputScheme::Pinyin, "xian");
    bool foundXianSingle = false; // e.g. 先, 现, 线
    bool foundXiAnDouble = false; // 西安
    for (const auto& c : qrXian.candidates) {
        if (c.text == L"先" || c.text == L"现" || c.text == L"线") foundXianSingle = true;
        if (c.text == L"西安") foundXiAnDouble = true;
    }
    TEST_ASSERT(foundXianSingle, "Continuous 'xian' must match single syllable words like '先'");
    TEST_ASSERT(foundXiAnDouble, "Continuous 'xian' must also match multi-syllable word '西安'");

    // 2. Query explicit separator "xi'an": MUST match "西安", but MUST NOT match "先"
    auto qrXiAnSep = engine.Query(InputScheme::Pinyin, "xi'an");
    bool foundInSep = false;
    bool singleWronglyFound = false;
    for (const auto& c : qrXiAnSep.candidates) {
        if (c.text == L"西安") foundInSep = true;
        if (c.text == L"先" || c.text == L"现" || c.text == L"线") singleWronglyFound = true;
    }
    TEST_ASSERT(foundInSep, "Explicit separator 'xi'an' must match '西安'");
    TEST_ASSERT(!singleWronglyFound, "Explicit separator 'xi'an' must NOT match single syllable words like '先'!");

    std::cout << "[PASS] Pinyin Ambiguity & Separator tests passed cleanly!\n";
    return 0;
}

// 2. Test Wubi Full Match vs Prefix Completion
int TestWubiFullVsPrefix(ImeEngine& engine) {
    std::cout << "[TEST] Testing Wubi86 Exact Match vs Prefix Completion..." << std::endl;

    // Exact match: "aaaa" matches "工" as ExactFull
    auto qrAaaa = engine.Query(InputScheme::Wubi86, "aaaa");
    TEST_ASSERT(!qrAaaa.candidates.empty(), "Query 'aaaa' must return candidates");
    TEST_ASSERT(qrAaaa.candidates[0].quality == MatchQuality::ExactFull, "Candidate for 'aaaa' must be ExactFull");
    TEST_ASSERT(qrAaaa.candidates[0].text == L"工", "Top candidate for 'aaaa' must be '工'");

    // Prefix match: "aa" has both exact (e.g. 2-key shorthand) and prefix completion (e.g. 4-key "aaaa")
    auto qrAa = engine.Query(InputScheme::Wubi86, "aa");
    TEST_ASSERT(!qrAa.candidates.empty(), "Query 'aa' must return candidates");

    // All ExactFull candidates must appear strictly before PrefixCompletion candidates
    bool seenPrefixCompletion = false;
    for (const auto& c : qrAa.candidates) {
        if (c.quality == MatchQuality::PrefixCompletion) {
            seenPrefixCompletion = true;
        } else if (c.quality == MatchQuality::ExactFull) {
            TEST_ASSERT(!seenPrefixCompletion, "ExactFull candidates must appear strictly before PrefixCompletion candidates!");
        }
    }
    TEST_ASSERT(seenPrefixCompletion, "Query 'aa' must include PrefixCompletion candidates");

    std::cout << "[PASS] Wubi86 Exact Match vs Prefix Completion passed!\n";
    return 0;
}

// 3. Test Multi-Code and Cross-Scheme Isolation
int TestMultiCodeAndCrossSchemeIsolation(ImeEngine& engine) {
    std::cout << "[TEST] Testing Multi-Code and Cross-Scheme Isolation..." << std::endl;

    // Add user entry with both Pinyin and Wubi codes
    std::wstring phrase = L"测试短语";
    std::string id = engine.AddUserEntry(phrase, {
        {InputScheme::Pinyin, "ceshiduanwu"},
        {InputScheme::Wubi86, "cesh"}
    });
    TEST_ASSERT(!id.empty(), "Must successfully add user entry");

    // Add custom shortcut: ";cs" -> "测试短语"
    std::string idCustom = engine.AddUserEntry(L"我的快捷地址", {
        {InputScheme::Custom, ";cs"}
    });
    TEST_ASSERT(!idCustom.empty(), "Must successfully add custom shortcut");

    // 1. Query Pinyin
    auto qrPinyin = engine.Query(InputScheme::Pinyin, "ceshiduanwu");
    bool foundPinyin = false;
    for (const auto& c : qrPinyin.candidates) {
        if (c.text == phrase) foundPinyin = true;
    }
    TEST_ASSERT(foundPinyin, "User entry must be retrievable via Pinyin");

    // 2. Query Wubi
    auto qrWubi = engine.Query(InputScheme::Wubi86, "cesh");
    bool foundWubi = false;
    for (const auto& c : qrWubi.candidates) {
        if (c.text == phrase) foundWubi = true;
    }
    TEST_ASSERT(foundWubi, "User entry must be retrievable via Wubi");

    // 3. Verify cross-scheme isolation: Pinyin code in Wubi mode must return 0
    auto qrCross = engine.Query(InputScheme::Wubi86, "ceshiduanwu");
    for (const auto& c : qrCross.candidates) {
        TEST_ASSERT(c.text != phrase, "Pinyin code must NOT trigger user entry in Wubi mode!");
    }

    // 4. Verify custom shortcut
    auto qrCustom = engine.Query(InputScheme::Custom, ";cs");
    TEST_ASSERT(!qrCustom.candidates.empty(), "Custom shortcut must be retrievable");
    TEST_ASSERT(qrCustom.candidates[0].text == L"我的快捷地址", "Custom shortcut content match");

    std::cout << "[PASS] Multi-code and cross-scheme isolation verified!\n";
    return 0;
}

// 4. Test Built-in & User Deduplication & Merging
int TestBuiltinAndUserDeduplication(ImeEngine& engine) {
    std::cout << "[TEST] Testing Built-in & User Entry Deduplication..." << std::endl;

    // Add user entry with exact same text as a builtin word ("中文")
    std::string id = engine.AddUserEntry(L"中文", {
        {InputScheme::Pinyin, "zhongwen"}
    });
    TEST_ASSERT(!id.empty(), "Add duplicate user entry succeeds");

    auto qr = engine.Query(InputScheme::Pinyin, "zhongwen");
    int zhongwenCount = 0;
    EntrySource sourceSeen = EntrySource::Builtin;
    for (const auto& c : qr.candidates) {
        if (c.text == L"中文") {
            zhongwenCount++;
            sourceSeen = c.source;
        }
    }
    TEST_ASSERT(zhongwenCount == 1, "Duplicate built-in and user entry must be merged into exactly 1 candidate!");
    TEST_ASSERT(sourceSeen == EntrySource::User, "Merged candidate must reflect user ownership");

    std::cout << "[PASS] Deduplication and merging verified!\n";
    return 0;
}

// 5. Test Pinned Order Immutability Against Frequency Learning
int TestPinnedOrderImmutability(ImeEngine& engine) {
    std::cout << "[TEST] Testing Pinned Order Immutability against High-Frequency Learning..." << std::endl;

    // Add two user entries under code "pintest"
    std::string idA = engine.AddUserEntry(L"置顶词条A", {{InputScheme::Pinyin, "pintest"}});
    std::string idB = engine.AddUserEntry(L"普通词条B", {{InputScheme::Pinyin, "pintest"}});

    // Pin Candidate A to Rank 1
    bool pinOk = engine.PinCandidate(InputScheme::Pinyin, "pintest", idA, 1);
    TEST_ASSERT(pinOk, "Pin Candidate A to rank 1");

    // Initially A is #1
    auto qr1 = engine.Query(InputScheme::Pinyin, "pintest");
    TEST_ASSERT(qr1.candidates.size() >= 2, "Must have 2 candidates");
    TEST_ASSERT(qr1.candidates[0].text == L"置顶词条A", "Pinned candidate A must be #1");

    // Simulate 100 commits to Candidate B (drastically boosting its frequency and score)
    for (int i = 0; i < 100; ++i) {
        engine.CommitCandidate("", InputScheme::Pinyin, "pintest", idB);
    }

    // Re-query: Candidate A MUST STILL BE #1! Pinned order cannot be overridden by frequency!
    auto qr2 = engine.Query(InputScheme::Pinyin, "pintest");
    TEST_ASSERT(qr2.candidates[0].text == L"置顶词条A", "CRITICAL: Pinned candidate A MUST STAY #1 despite 100 commits to B!");
    TEST_ASSERT(qr2.candidates[0].is_pinned, "Candidate A must be marked as pinned");
    TEST_ASSERT(qr2.candidates[1].text == L"普通词条B", "Candidate B must follow pinned A");

    std::cout << "[PASS] Pinned order immutability verified!\n";
    return 0;
}

// 6. Test Frequent Usage Progression ("常用逐渐靠前")
int TestFrequentUsageProgression(ImeEngine& engine) {
    std::cout << "[TEST] Testing Frequent Usage Progression ('常用逐渐靠前')..." << std::endl;

    std::string idX = engine.AddUserEntry(L"候选项X", {{InputScheme::Pinyin, "ranktest"}});
    std::string idY = engine.AddUserEntry(L"候选项Y", {{InputScheme::Pinyin, "ranktest"}});

    // Initially tie-break or insert order determines ranking
    auto qrInit = engine.Query(InputScheme::Pinyin, "ranktest");
    TEST_ASSERT(qrInit.candidates.size() >= 2, "Must have 2 candidates");

    // Choose the candidate currently in 2nd place and commit it multiple times
    std::string secondPlaceId = qrInit.candidates[1].entry_id;
    std::wstring secondPlaceText = qrInit.candidates[1].text;

    for (int i = 0; i < 5; ++i) {
        engine.CommitCandidate("", InputScheme::Pinyin, "ranktest", secondPlaceId);
    }

    // Re-query: The second-place candidate must now move forward to #1!
    auto qrAfter = engine.Query(InputScheme::Pinyin, "ranktest");
    TEST_ASSERT(qrAfter.candidates[0].entry_id == secondPlaceId, "Repeatedly used candidate must move ahead to #1!");
    TEST_ASSERT(qrAfter.candidates[0].text == secondPlaceText, "Text matches newly promoted #1");

    std::cout << "[PASS] Frequent usage progression verified!\n";
    return 0;
}

// 7. Test Non-Commit Path Learning Guard & Idempotency
int TestLearningGuardAndIdempotency(ImeEngine& engine) {
    std::cout << "[TEST] Testing Non-Commit Learning Guard and Idempotency..." << std::endl;

    std::string id = engine.AddUserEntry(L"防护词条", {{InputScheme::Pinyin, "guardtest"}});

    // 1. Perform 100 queries: stats must NOT increase
    for (int i = 0; i < 100; ++i) {
        engine.Query(InputScheme::Pinyin, "guardtest");
    }
    auto qr1 = engine.Query(InputScheme::Pinyin, "guardtest");
    TEST_ASSERT(qr1.candidates[0].selected_count == 0, "Querying must NEVER increment selected_count (0 expected)!");

    // 2. Commit with event ID "evt_unique_1"
    bool c1 = engine.CommitCandidate("evt_unique_1", InputScheme::Pinyin, "guardtest", id);
    TEST_ASSERT(c1, "First commit with event ID succeeds");

    auto qr2 = engine.Query(InputScheme::Pinyin, "guardtest");
    TEST_ASSERT(qr2.candidates[0].selected_count == 1, "Selected count must be exactly 1 after first commit");

    // 3. Retry commit with the exact same event ID "evt_unique_1" (simulating IPC retry)
    bool c2 = engine.CommitCandidate("evt_unique_1", InputScheme::Pinyin, "guardtest", id);
    TEST_ASSERT(c2, "Duplicate commit with same event ID succeeds idempotently");

    auto qr3 = engine.Query(InputScheme::Pinyin, "guardtest");
    TEST_ASSERT(qr3.candidates[0].selected_count == 1, "CRITICAL: Duplicate event ID must NOT increase selected_count (still 1)!");

    std::cout << "[PASS] Learning guard & idempotency verified!\n";
    return 0;
}

// 8. Test Clock Injection & Habit Decay
int TestClockInjectionAndDecay() {
    std::cout << "[TEST] Testing Clock Injection and Half-Life Decay..." << std::endl;

    auto clock = std::make_shared<MockClock>(1700000000);
    ImeEngine engine;
    TEST_ASSERT(engine.Initialize(":memory:", "", clock), "Initialize engine with mock clock");

    std::string id = engine.AddUserEntry(L"衰减测试", {{InputScheme::Pinyin, "decaytest"}});

    // Commit 10 times at t0 = 1700000000
    for (int i = 0; i < 10; ++i) {
        engine.CommitCandidate("", InputScheme::Pinyin, "decaytest", id);
    }

    auto qr0 = engine.Query(InputScheme::Pinyin, "decaytest");
    TEST_ASSERT(qr0.candidates[0].selected_count == 10, "Selected count is 10");
    TEST_ASSERT(std::abs(qr0.candidates[0].effective_count - 10.0) < 0.1, "Initial effective count is ~10.0");

    // Advance clock by 30 days (1 half-life = 30 * 86400 seconds)
    clock->Advance(30 * 86400);

    auto qr1 = engine.Query(InputScheme::Pinyin, "decaytest");
    // selected_count remains 10 (historical record)
    TEST_ASSERT(qr1.candidates[0].selected_count == 10, "Selected count is unchanged");
    // effective_count must decay by 50% to ~5.0!
    std::cout << "  Decayed effective count after 30 days: " << qr1.candidates[0].effective_count << " (expected ~5.0)\n";
    TEST_ASSERT(qr1.candidates[0].effective_count >= 4.9 && qr1.candidates[0].effective_count <= 5.1,
                "Effective count must decay to ~5.0 after 30 days");

    // Advance clock by another 30 days (2 half-lives total)
    clock->Advance(30 * 86400);
    auto qr2 = engine.Query(InputScheme::Pinyin, "decaytest");
    std::cout << "  Decayed effective count after 60 days: " << qr2.candidates[0].effective_count << " (expected ~2.5)\n";
    TEST_ASSERT(qr2.candidates[0].effective_count >= 2.4 && qr2.candidates[0].effective_count <= 2.6,
                "Effective count must decay to ~2.5 after 60 days");

    std::cout << "[PASS] Clock injection & habit decay verified!\n";
    return 0;
}

// 9. Test Restart Persistence & Import Rollback on Failure
int TestPersistenceAndImportRollback() {
    std::cout << "[TEST] Testing Restart Persistence & Import Rollback on Failure..." << std::endl;

    std::string testDb = "test_persistence.db";
    if (std::filesystem::exists(testDb)) {
        std::filesystem::remove(testDb);
    }

    std::string testId;
    {
        ImeEngine engine;
        TEST_ASSERT(engine.Initialize(testDb, ""), "Open persistence DB");
        testId = engine.AddUserEntry(L"持久化测试词", {{InputScheme::Pinyin, "perstest"}});
        TEST_ASSERT(!testId.empty(), "Add entry");
        engine.PinCandidate(InputScheme::Pinyin, "perstest", testId, 1);
        engine.CommitCandidate("", InputScheme::Pinyin, "perstest", testId);
    } // Engine closed and destroyed here

    // Reopen new engine pointing to the same database
    {
        ImeEngine engineReopen;
        TEST_ASSERT(engineReopen.Initialize(testDb, ""), "Reopen persistence DB");
        auto qr = engineReopen.Query(InputScheme::Pinyin, "perstest");
        TEST_ASSERT(!qr.candidates.empty(), "Candidate must survive restart");
        TEST_ASSERT(qr.candidates[0].text == L"持久化测试词", "Text preserved after restart");
        TEST_ASSERT(qr.candidates[0].is_pinned, "Pinned status preserved after restart");
        TEST_ASSERT(qr.candidates[0].selected_count == 1, "Commit stats preserved after restart");

        // 1. Export valid JSON
        std::string jsonPath = "test_export.json";
        TEST_ASSERT(engineReopen.ExportData(jsonPath), "Export to JSON");
        TEST_ASSERT(std::filesystem::exists(jsonPath), "Export file exists");

        // 2. Corrupt JSON and attempt import -> MUST ROLLBACK!
        std::string badJsonPath = "test_corrupted.json";
        {
            std::ofstream f(badJsonPath);
            f << "{ \"version\": \"2.0\", \"entries\": [ { \"text\": \"非法\" } ] }"; // unsupported version!
        }
        std::string err;
        bool importBad = engineReopen.ImportData(badJsonPath, true, &err);
        TEST_ASSERT(!importBad, "Import of bad version must fail");
        std::cout << "  Expected import error caught: " << err << "\n";

        // Verify existing database data was NOT wiped or corrupted
        auto qrAfterBad = engineReopen.Query(InputScheme::Pinyin, "perstest");
        TEST_ASSERT(!qrAfterBad.candidates.empty(), "Original data must be intact after import failure rollback!");

        // 3. Test valid JSON import in Replace mode
        bool importGood = engineReopen.ImportData(jsonPath, true, &err);
        TEST_ASSERT(importGood, "Import of valid JSON must succeed");

        std::filesystem::remove(jsonPath);
        std::filesystem::remove(badJsonPath);
    }

    std::filesystem::remove(testDb);
    std::filesystem::remove(testDb + "-wal");
    std::filesystem::remove(testDb + "-shm");

    std::cout << "[PASS] Restart persistence & import rollback verified!\n";
    return 0;
}

// 10. Test Concurrent Queries & Learning Updates Consistency
int TestConcurrentQueriesAndUpdates(ImeEngine& engine) {
    std::cout << "[TEST] Testing Concurrent Queries & Learning Updates Consistency..." << std::endl;

    std::atomic<bool> startFlag{false};
    std::atomic<bool> stopFlag{false};
    std::atomic<int> queryCount{0};
    std::atomic<int> commitCount{0};

    std::string id = engine.AddUserEntry(L"并发词条", {{InputScheme::Pinyin, "conctest"}});

    std::vector<std::thread> queryThreads;
    for (int i = 0; i < 4; ++i) {
        queryThreads.emplace_back([&]() {
            while (!startFlag.load()) std::this_thread::yield();
            while (!stopFlag.load()) {
                auto qr = engine.Query(InputScheme::Pinyin, "conctest");
                if (!qr.candidates.empty()) {
                    queryCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> commitThreads;
    for (int i = 0; i < 2; ++i) {
        commitThreads.emplace_back([&, i]() {
            while (!startFlag.load()) std::this_thread::yield();
            for (int k = 0; k < 50; ++k) {
                std::string evt = "evt_conc_" + std::to_string(i) + "_" + std::to_string(k);
                if (engine.CommitCandidate(evt, InputScheme::Pinyin, "conctest", id)) {
                    commitCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    startFlag.store(true);

    for (auto& t : commitThreads) {
        t.join();
    }
    stopFlag.store(true);
    for (auto& t : queryThreads) {
        t.join();
    }

    std::cout << "  Concurrent test finished: " << queryCount.load() << " queries, " << commitCount.load() << " commits.\n";
    TEST_ASSERT(commitCount.load() == 100, "All 100 commits succeeded");

    auto qrFinal = engine.Query(InputScheme::Pinyin, "conctest");
    TEST_ASSERT(qrFinal.candidates[0].selected_count == 100, "Final selected count must be exactly 100");

    std::cout << "[PASS] Concurrent queries & learning updates consistency verified!\n";
    return 0;
}

// 12. Review Round 1 Negative Probe Suite
int TestReviewRound1NegativeCases() {
    std::cout << "[TEST] Running Review Round 1 Negative & Integrity Probe Suite..." << std::endl;

    // 1. Failed learning must not consume event or report success
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Init in-memory engine");
        auto id = e.AddUserEntry(L"word", {{InputScheme::Custom, ";good"}});
        bool invalid = e.CommitCandidate("same-event", InputScheme::Custom, ";good", "missing-entry");
        TEST_ASSERT(!invalid, "Commit on missing entry must fail");
        bool retry = e.CommitCandidate("same-event", InputScheme::Custom, ";good", id);
        TEST_ASSERT(retry, "Valid retry of same event must succeed");
        int count = e.Query(InputScheme::Custom, ";good").candidates.at(0).selected_count;
        TEST_ASSERT(count == 1, "Selected count must be 1 after valid retry");
        bool unrelated = e.CommitCandidate("unrelated-event", InputScheme::Pinyin, "not-the-code", id);
        TEST_ASSERT(!unrelated, "Commit with unrelated scheme/code must be rejected");
    }

    // 2. Multi-code aliases for same entry must not multiply stats or match wildcards
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Init in-memory engine");
        auto id = e.AddUserEntry(L"same-word", {{InputScheme::Custom, ";abc"}, {InputScheme::Custom, ";abd"}});
        e.CommitCandidate("one-event", InputScheme::Custom, ";a", id);
        auto q = e.Query(InputScheme::Custom, ";a");
        TEST_ASSERT(!q.candidates.empty(), "Candidate found under prefix ;a");
        TEST_ASSERT(q.candidates.at(0).selected_count == 1, "Count must be exactly 1, not multiplied by alias count");

        auto wildcard = e.Query(InputScheme::Custom, ";%");
        TEST_ASSERT(wildcard.candidates.empty(), "Literal '% 'must not act as SQL wildcard");
    }

    // 3. High-fidelity decay preservation and idempotent merge import
    {
        auto clock = std::make_shared<MockClock>();
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", "", clock), "Init mock clock engine");
        auto id = e.AddUserEntry(L"decay", {{InputScheme::Custom, ";decay"}});
        for (int i = 0; i < 10; ++i) {
            e.CommitCandidate("decay-" + std::to_string(i), InputScheme::Custom, ";decay", id);
        }
        clock->Advance(30LL * 86400);
        double before = e.Query(InputScheme::Custom, ";decay").candidates.at(0).effective_count;
        TEST_ASSERT(std::abs(before - 5.0) < 0.1, "Effective count decayed to 5.0 before export");

        std::string jsonPath = "test_review_decay.json";
        TEST_ASSERT(e.ExportData(jsonPath), "Export decay test data");

        ImeEngine restored;
        TEST_ASSERT(restored.Initialize(":memory:", "", clock), "Init restored engine");
        TEST_ASSERT(restored.ImportData(jsonPath, false), "Import decay test data");
        double after = restored.Query(InputScheme::Custom, ";decay").candidates.at(0).effective_count;
        TEST_ASSERT(std::abs(after - 5.0) < 0.1, "Effective count must faithfully remain 5.0 after import");

        // Second import (merge) must NOT double count to 20
        TEST_ASSERT(restored.ImportData(jsonPath, false), "Second merge import");
        int merged = restored.Query(InputScheme::Custom, ";decay").candidates.at(0).selected_count;
        TEST_ASSERT(merged == 10, "Merge mode must be idempotent (count stays 10, not 20)");

        std::filesystem::remove(jsonPath);
    }

    // 4. Existing ID code replacement and invalid replace rollback
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Init engine");
        auto id = e.AddUserEntry(L"old", {{InputScheme::Custom, ";old"}});

        std::string mergeJson = "test_merge_codes.json";
        {
            std::ofstream f(mergeJson);
            f << "{\n"
                 "  \"version\": \"1.0\",\n"
                 "  \"entries\": [\n"
                 "    {\n"
                 "      \"id\": \"" << id << "\",\n"
                 "      \"text\": \"new\",\n"
                 "      \"codes\": [{\"scheme\": \"custom\", \"code\": \";new\"}]\n"
                 "    }\n"
                 "  ]\n"
                 "}";
        }
        TEST_ASSERT(e.ImportData(mergeJson, false), "Import merge codes");
        auto nq = e.Query(InputScheme::Custom, ";new");
        TEST_ASSERT(!nq.candidates.empty(), "Query for new code must find entry");
        TEST_ASSERT(nq.candidates[0].text == L"new", "Updated text matches");

        std::string invalidJson = "test_invalid_replace.json";
        {
            std::ofstream f(invalidJson);
            f << "{\n"
                 "  \"version\": \"1.0\",\n"
                 "  \"entries\": [\n"
                 "    {\n"
                 "      \"id\": \"invalid-entry\",\n"
                 "      \"text\": \"fail\",\n"
                 "      \"codes\": [{\"scheme\": \"typo-scheme\", \"code\": \"???\"}]\n"
                 "    }\n"
                 "  ]\n"
                 "}";
        }
        std::string err;
        bool invalidImport = e.ImportData(invalidJson, true, &err);
        TEST_ASSERT(!invalidImport, "Invalid scheme/code replace import must fail");
        TEST_ASSERT(e.GetUserEntry(id).has_value(), "Original entry must be 100% intact after failed replace import rollback");

        std::filesystem::remove(mergeJson);
        std::filesystem::remove(invalidJson);
    }

    // 5. Builtin word pin & learning stats survive backup
    {
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", "data/dictionaries"), "Init engine with dicts");
        auto q = e.Query(InputScheme::Pinyin, "zhongwen");
        TEST_ASSERT(!q.candidates.empty(), "Builtin word found");
        auto id = q.candidates[0].entry_id;

        TEST_ASSERT(e.PinCandidate(InputScheme::Pinyin, "zhongwen", id, 1), "Pin builtin candidate");
        TEST_ASSERT(e.CommitCandidate("builtin-event", InputScheme::Pinyin, "zhongwen", id), "Commit builtin candidate");

        std::string builtinJson = "test_builtin_backup.json";
        TEST_ASSERT(e.ExportData(builtinJson), "Export builtin backup");

        ImeEngine restored;
        TEST_ASSERT(restored.Initialize(":memory:", "data/dictionaries"), "Init restored engine");
        TEST_ASSERT(restored.ImportData(builtinJson, false), "Import builtin backup");

        auto rq = restored.Query(InputScheme::Pinyin, "zhongwen");
        bool pinned = false;
        int count = 0;
        for (const auto& c : rq.candidates) {
            if (c.entry_id == id) {
                pinned = c.is_pinned;
                count = c.selected_count;
                break;
            }
        }
        TEST_ASSERT(pinned, "Builtin word pinned status must be preserved");
        TEST_ASSERT(count == 1, "Builtin word commit count must be preserved");

        std::filesystem::remove(builtinJson);
    }

    std::cout << "[PASS] All Review Round 1 negative & data integrity probes PASSED cleanly!\n";
    return 0;
}

static int TestReviewRound2Probes() {
    std::cout << "[TEST] Running Review Round 2 Data Integrity & Architecture Suite..." << std::endl;

    // 1. Write Failure Aborts Replace & Rolls Back Atomically
    {
        std::cout << "  Subtest 1: Write failure rollback..." << std::endl;
        SqliteDb db;
        TEST_ASSERT(db.Open(":memory:"), "Open in-memory db for write failure test");
        TEST_ASSERT(Migration::Migrate(db), "Migrate db");
        auto clock = std::make_shared<MockClock>();
        EntryRepository repo(db, clock);

        auto oldId = repo.AddEntry(L"old_data", {{InputScheme::Custom, ";old"}});
        TEST_ASSERT(!oldId.empty(), "Old entry created");

        // Install a trigger that simulates SQL execution failure on specific code insert
        TEST_ASSERT(db.Execute("CREATE TRIGGER fail_blocked BEFORE INSERT ON entry_codes WHEN NEW.code=';blocked' BEGIN SELECT RAISE(ABORT,'injected-code-failure'); END;"), "Create failure trigger");

        nlohmann::json j = {
            {"version", "1.0"},
            {"entries", nlohmann::json::array({
                {
                    {"id", "incoming"},
                    {"text", "incoming"},
                    {"codes", nlohmann::json::array({
                        {{"scheme", "custom"}, {"code", ";blocked"}}
                    })}
                }
            })}
        };
        std::ofstream f("test_r2_write_failure.json");
        f << j.dump(2);
        f.close();

        std::string err;
        bool ok = JsonBackup::ImportFromFile(repo, db, "test_r2_write_failure.json", true, &err);
        TEST_ASSERT(!ok, "Import must report failure on statement error");
        TEST_ASSERT(repo.GetEntry(oldId).has_value(), "Old entry must be preserved on rollback");
        TEST_ASSERT(!repo.GetEntry("incoming").has_value(), "Partial incoming entry must not exist");

        std::filesystem::remove("test_r2_write_failure.json");
    }

    // 2. Global Metadata Pre-Validation Rejects Orphans Before DB Mutation
    {
        std::cout << "  Subtest 2: Pre-validation..." << std::endl;
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Init engine");
        auto oldId = e.AddUserEntry(L"preval_old", {{InputScheme::Custom, ";preval"}});
        TEST_ASSERT(!oldId.empty(), "Old entry created");

        nlohmann::json j = {
            {"version", "1.0"},
            {"entries", nlohmann::json::array()},
            {"stats", nlohmann::json::array({
                {{"scheme", "custom"}, {"code", ";missing"}, {"entry_id", "dangling_orphan_id"}, {"count", 1}}
            })}
        };
        std::ofstream f("test_r2_orphan.json");
        f << j.dump(2);
        f.close();

        std::string err;
        bool ok = e.ImportData("test_r2_orphan.json", true, &err);
        TEST_ASSERT(!ok, "Import must be rejected by pre-validation");
        TEST_ASSERT(e.GetUserEntry(oldId).has_value(), "Existing database must not be touched on validation failure");

        std::filesystem::remove("test_r2_orphan.json");
    }

    // 3. Replace Discards Builtin Preferences Absent from Replacement Backup
    {
        std::cout << "  Subtest 3: Replace discards builtin habits..." << std::endl;
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", "data/dictionaries"), "Init engine with dicts");
        auto q = e.Query(InputScheme::Pinyin, "zhongwen");
        TEST_ASSERT(!q.candidates.empty(), "Query builtin word");
        auto id = q.candidates[0].entry_id;

        TEST_ASSERT(e.PinCandidate(InputScheme::Pinyin, "zhongwen", id, 1), "Pin builtin candidate");
        TEST_ASSERT(e.CommitCandidate("r2-replace-commit", InputScheme::Pinyin, "zhongwen", id), "Commit builtin candidate");

        nlohmann::json emptyBackup = {
            {"version", "1.0"},
            {"entries", nlohmann::json::array()},
            {"preferences", nlohmann::json::array()},
            {"stats", nlohmann::json::array()}
        };
        std::ofstream f("test_r2_empty.json");
        f << emptyBackup.dump(2);
        f.close();

        std::string err;
        TEST_ASSERT(e.ImportData("test_r2_empty.json", true, &err), "Empty replace import succeeds");

        auto rq = e.Query(InputScheme::Pinyin, "zhongwen");
        for (const auto& c : rq.candidates) {
            if (c.entry_id == id) {
                TEST_ASSERT(!c.is_pinned, "Builtin word pin must be cleared in replace mode");
                TEST_ASSERT(c.selected_count == 0, "Builtin word count must be cleared in replace mode");
            }
        }

        std::filesystem::remove("test_r2_empty.json");
    }

    // 4. Unified Timeline Decay Merge Prevents Stale Habits from Overwriting Recent Usage
    {
        std::cout << "  Subtest 4: Decay merge..." << std::endl;
        auto clock = std::make_shared<MockClock>();
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", "", clock), "Init engine with mock clock");
        auto id = e.AddUserEntry(L"decay_entry", {{InputScheme::Custom, ";decay"}});

        for (int i = 0; i < 10; ++i) {
            e.CommitCandidate("old-commit-" + std::to_string(i), InputScheme::Custom, ";decay", id);
        }
        std::string oldJson = "test_r2_decay_old.json";
        TEST_ASSERT(e.ExportData(oldJson), "Export old backup at t=0");

        // Advance 30 days (1 half-life), then commit once more
        clock->Advance(30LL * 86400);
        TEST_ASSERT(e.CommitCandidate("new-commit", InputScheme::Custom, ";decay", id), "Commit after 30 days");
        double effBefore = e.Query(InputScheme::Custom, ";decay").candidates.at(0).effective_count;
        TEST_ASSERT(std::abs(effBefore - 6.0) < 0.01, "Effective count before merge should be 10*0.5 + 1 = 6.0");

        // Merge old backup: should evaluate both on common timeline and NOT bump effective_count to 10
        TEST_ASSERT(e.ImportData(oldJson, false), "Merge old backup");
        auto candAfter = e.Query(InputScheme::Custom, ";decay").candidates.at(0);
        TEST_ASSERT(std::abs(candAfter.effective_count - 6.0) < 0.01, "Effective count after merge must remain 6.0, not stale 10.0");
        TEST_ASSERT(candAfter.selected_count == 11, "Selected count is max(10, 11) = 11");

        std::filesystem::remove(oldJson);
    }

    // 5. Schema Migration Upgrades V1 Commit Events Table Smoothly
    {
        std::cout << "  Subtest 5: Schema migration..." << std::endl;
        const char* dbFile = "test_r2_migrated.db";
        std::error_code ec;
        std::filesystem::remove(dbFile, ec);
        {
            SqliteDb db;
            TEST_ASSERT(db.Open(dbFile), "Open db for v1 setup");
            TEST_ASSERT(Migration::Migrate(db), "Migrate initial schema");
            // Emulate v1 commit_events structure lacking scheme/code/entry_id
            TEST_ASSERT(db.Execute("DROP TABLE commit_events; CREATE TABLE commit_events(event_id TEXT PRIMARY KEY, committed_at INTEGER NOT NULL);"), "Create v1 schema state");
        }

        {
            ImeEngine e;
            TEST_ASSERT(e.Initialize(dbFile, ""), "Initialize engine triggers automatic migration");
            auto id = e.AddUserEntry(L"migrated_word", {{InputScheme::Custom, ";migrated"}});
            TEST_ASSERT(e.CommitCandidate("event_after_migration", InputScheme::Custom, ";migrated", id), "Commit candidate succeeds on migrated database");
            auto q = e.Query(InputScheme::Custom, ";migrated");
            TEST_ASSERT(!q.candidates.empty() && q.candidates[0].selected_count == 1, "Candidate count recorded after migration");
        }

        std::filesystem::remove(dbFile, ec);
    }

    // 6. Pinned Candidates Recall Beyond Prefix Pool Bounds
    {
        std::cout << "  Subtest 6: Pinned recall beyond pool..." << std::endl;
        std::filesystem::create_directory("test_r2_dicts");
        std::ofstream f("test_r2_dicts/wubi86.dict");
        std::string targetCode, targetText;
        for (int i = 0; i < 600; ++i) {
            std::string code = "aaaa";
            code[1] = 'a' + i / 625;
            code[2] = 'a' + (i / 25) % 25;
            code[3] = 'a' + i % 25;
            std::string text = "word" + std::to_string(i);
            f << text << '\t' << code << "\t10\n";
            targetCode = code;
            targetText = text;
        }
        f.close();

        {
            ImeEngine e;
            TEST_ASSERT(e.Initialize(":memory:", "test_r2_dicts"), "Init engine with 600-word dict");
            auto qTarget = e.Query(InputScheme::Wubi86, targetCode);
            TEST_ASSERT(!qTarget.candidates.empty(), "Query last entry in dict");
            auto targetId = qTarget.candidates[0].entry_id;

            TEST_ASSERT(e.PinCandidate(InputScheme::Wubi86, "a", targetId, 1), "Pin candidate beyond pool bounds");
            auto qPrefix = e.Query(InputScheme::Wubi86, "a");
            TEST_ASSERT(!qPrefix.candidates.empty(), "Query prefix 'a'");
            TEST_ASSERT(qPrefix.candidates[0].entry_id == targetId, "Pinned candidate beyond pool must be recalled as top candidate");
            TEST_ASSERT(qPrefix.candidates[0].is_pinned, "Pinned candidate must have is_pinned = true");
        }

        std::error_code ec;
        std::filesystem::remove_all("test_r2_dicts", ec);
    }

    // 7. User Entry Text & Codes Editing Contract
    {
        std::cout << "  Subtest 7: Entry text & codes editing..." << std::endl;
        ImeEngine e;
        TEST_ASSERT(e.Initialize(":memory:", ""), "Init engine");
        auto id = e.AddUserEntry(L"initial", {{InputScheme::Custom, ";init"}});
        TEST_ASSERT(!id.empty(), "Add entry");

        // Edit text and update codes
        TEST_ASSERT(e.EditUserEntry(id, L"updated", {{InputScheme::Custom, ";newcode"}, {InputScheme::Pinyin, "xinde"}}), "Edit entry text and codes");
        auto entryOpt = e.GetUserEntry(id);
        TEST_ASSERT(entryOpt.has_value(), "Entry exists");
        TEST_ASSERT(entryOpt->text == L"updated", "Text updated");
        TEST_ASSERT(entryOpt->codes.size() == 2, "Codes updated to 2");

        auto qOld = e.Query(InputScheme::Custom, ";init");
        TEST_ASSERT(qOld.candidates.empty(), "Old code query empty");

        auto qNew = e.Query(InputScheme::Custom, ";newcode");
        TEST_ASSERT(!qNew.candidates.empty() && qNew.candidates[0].entry_id == id, "New code query returns updated entry");
    }

    std::cout << "[PASS] All Review Round 2 probes and regressions PASSED cleanly!\n";
    return 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "=== Running PersonalIME Stage 2 (ime-core) Test Suite ===" << std::endl;

    std::cout << "Step 1: Initializing ImeEngine..." << std::endl;
    ImeEngine engine;
    if (!engine.Initialize(":memory:", "data/dictionaries")) {
        std::cerr << "[FATAL] Failed to initialize in-memory test engine!" << std::endl;
        return 1;
    }
    std::cout << "Step 1: ImeEngine initialized successfully! (Wubi: "
              << engine.GetBuiltinDict().WubiEntryCount() << ", Pinyin: "
              << engine.GetBuiltinDict().PinyinEntryCount() << ")" << std::endl;

    std::cout << "Step 2: Starting TestPinyinAmbiguityAndSeparators..." << std::endl;
    if (TestPinyinAmbiguityAndSeparators(engine) != 0) return 1;
    std::cout << "Step 3: Starting TestWubiFullVsPrefix..." << std::endl;
    if (TestWubiFullVsPrefix(engine) != 0) return 1;
    std::cout << "Step 4: Starting TestMultiCodeAndCrossSchemeIsolation..." << std::endl;
    if (TestMultiCodeAndCrossSchemeIsolation(engine) != 0) return 1;
    std::cout << "Step 5: Starting TestBuiltinAndUserDeduplication..." << std::endl;
    if (TestBuiltinAndUserDeduplication(engine) != 0) return 1;
    std::cout << "Step 6: Starting TestPinnedOrderImmutability..." << std::endl;
    if (TestPinnedOrderImmutability(engine) != 0) return 1;
    std::cout << "Step 7: Starting TestFrequentUsageProgression..." << std::endl;
    if (TestFrequentUsageProgression(engine) != 0) return 1;
    std::cout << "Step 8: Starting TestLearningGuardAndIdempotency..." << std::endl;
    if (TestLearningGuardAndIdempotency(engine) != 0) return 1;
    std::cout << "Step 9: Starting TestClockInjectionAndDecay..." << std::endl;
    if (TestClockInjectionAndDecay() != 0) return 1;
    std::cout << "Step 10: Starting TestPersistenceAndImportRollback..." << std::endl;
    if (TestPersistenceAndImportRollback() != 0) return 1;
    std::cout << "Step 11: Starting TestConcurrentQueriesAndUpdates..." << std::endl;
    if (TestConcurrentQueriesAndUpdates(engine) != 0) return 1;
    std::cout << "Step 12: Starting TestReviewRound1NegativeCases..." << std::endl;
    if (TestReviewRound1NegativeCases() != 0) return 1;
    std::cout << "Step 13: Starting TestReviewRound2Probes..." << std::endl;
    if (TestReviewRound2Probes() != 0) return 1;

    std::cout << "=== ALL Stage 2 Test Suites (13/13) PASSED WITH ZERO ERRORS! ===" << std::endl;
    return 0;
}
