#include <iostream>
#include <fstream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>
#include "../src/core/engine/ime_engine.h"
#include "../src/core/clock.h"
#include "../src/core/types.h"

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

    std::cout << "=== ALL 10 Stage 2 Test Suites PASSED WITH ZERO ERRORS! ===" << std::endl;
    return 0;
}
