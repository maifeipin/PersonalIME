#include "../src/core/strategy/input_coordinator.h"
#include "../src/core/strategy/input_policy.h"
#include "../src/core/strategy/input_policy_json.h"
#include "../src/core/encoding/shuangpin_decoder.h"
#include "../src/core/storage/sqlite_db.h"
#include "../src/core/storage/entry_repository.h"
#include "../src/core/storage/migration.h"
#include "../src/core/dictionary/builtin_dict.h"
#include "../src/core/ranking/ranker.h"
#include "../src/core/types.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <vector>
#include <string>
#include <thread>
#include <future>
#include <fstream>
#include <chrono>

using namespace ime;

// Test assertion utility
static void AssertCondition(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[ASSERT FAILED] " << msg << std::endl;
        std::exit(1);
    }
}

// -----------------------------------------------------------------------------
// Test 1: Shuangpin Independent Standard Test Vectors (Contract 6)
// Sources:
// - Zrm 2000 Official Specification (自然码 2000 标准编码规约)
// - Microsoft Pinyin Input Method Reference (Windows 微软双拼键位规范)
// - Flypy Official Layout Reference (小鹤双拼官方音形查对表 flypy.cc)
// -----------------------------------------------------------------------------
static void TestShuangpinStandardVectors() {
    std::cout << "[TEST 1] Testing Shuangpin Independent Standard Vectors..." << std::endl;

    // 1. Natural Code (自然码)
    {
        ShuangpinDecoder zrm(ShuangpinLayout::Natural);
        // Zero-initials
        AssertCondition(zrm.DecodePair('a', 'a') == std::vector<std::string>{"a"}, "ZRM zero-initial 'aa' -> 'a'");
        AssertCondition(zrm.DecodePair('o', 'o') == std::vector<std::string>{"o"}, "ZRM zero-initial 'oo' -> 'o'");
        AssertCondition(zrm.DecodePair('e', 'e') == std::vector<std::string>{"e"}, "ZRM zero-initial 'ee' -> 'e'");
        AssertCondition(zrm.DecodePair('a', 'i') == std::vector<std::string>{"ai"}, "ZRM zero-initial 'ai' -> 'ai'");
        AssertCondition(zrm.DecodePair('a', 'n') == std::vector<std::string>{"an"}, "ZRM zero-initial 'an' -> 'an'");
        AssertCondition(zrm.DecodePair('a', 'h') == std::vector<std::string>{"ang"}, "ZRM zero-initial 'ah' -> 'ang'");
        AssertCondition(zrm.DecodePair('e', 'g') == std::vector<std::string>{"eng"}, "ZRM zero-initial 'eg' -> 'eng'");

        // Composite initials
        AssertCondition(zrm.DecodePair('v', 's') == std::vector<std::string>{"zhong"}, "ZRM 'vs' -> 'zhong'");
        AssertCondition(zrm.DecodePair('i', 'u') == std::vector<std::string>{"chu"}, "ZRM 'iu' -> 'chu'");
        AssertCondition(zrm.DecodePair('u', 'h') == std::vector<std::string>{"shang"}, "ZRM 'uh' -> 'shang'");

        // Disambiguation of dual-final keys: 'd' = iang / uang
        auto d1 = zrm.DecodePair('g', 'd'); // g + uang -> guang
        AssertCondition(d1 == std::vector<std::string>{"guang"}, "ZRM 'gd' -> 'guang'");
        auto d2 = zrm.DecodePair('x', 'd'); // x + iang -> xiang
        AssertCondition(d2 == std::vector<std::string>{"xiang"}, "ZRM 'xd' -> 'xiang'");

        // Multi-syllable & odd length
        auto m = zrm.Decode("vswf");
        AssertCondition(m.is_valid && m.is_complete, "ZRM 'vswf' valid and complete");
        AssertCondition(m.pinyin_spaced == "zhong wen", "ZRM 'vswf' -> 'zhong wen'");

        auto odd = zrm.Decode("vsw");
        AssertCondition(odd.is_valid && !odd.is_complete, "ZRM odd 'vsw' valid but incomplete");
        AssertCondition(odd.trailing_incomplete == "w", "ZRM odd trailing incomplete 'w'");
        AssertCondition(odd.pinyin_continuous == "zhongw", "ZRM odd 'vsw' continuous resolves to 'zhongw'");

        auto sep = zrm.Decode("xi'an");
        AssertCondition(sep.is_valid && sep.pinyin_spaced == "xi an", "ZRM separator 'xi'an' -> 'xi an'");
    }

    // 2. Microsoft Shuangpin (微软双拼)
    {
        ShuangpinDecoder ms(ShuangpinLayout::Microsoft);
        // Zero-initials with leading 'o'
        AssertCondition(ms.DecodePair('o', 'a') == std::vector<std::string>{"a"}, "MS zero-initial 'oa' -> 'a'");
        AssertCondition(ms.DecodePair('o', 'o') == std::vector<std::string>{"o"}, "MS zero-initial 'oo' -> 'o'");
        AssertCondition(ms.DecodePair('o', 'e') == std::vector<std::string>{"e"}, "MS zero-initial 'oe' -> 'e'");
        AssertCondition(ms.DecodePair('o', 'l') == std::vector<std::string>{"ai"}, "MS zero-initial 'ol' -> 'ai'");
        AssertCondition(ms.DecodePair('o', 'j') == std::vector<std::string>{"an"}, "MS zero-initial 'oj' -> 'an'");
        AssertCondition(ms.DecodePair('o', 'h') == std::vector<std::string>{"ang"}, "MS zero-initial 'oh' -> 'ang'");
        AssertCondition(ms.DecodePair('o', 'r') == std::vector<std::string>{"er"}, "MS zero-initial 'or' -> 'er'");

        // Composite initials
        AssertCondition(ms.DecodePair('v', 's') == std::vector<std::string>{"zhong"}, "MS 'vs' -> 'zhong'");
        AssertCondition(ms.DecodePair('i', 'u') == std::vector<std::string>{"chu"}, "MS 'iu' -> 'chu'");
        AssertCondition(ms.DecodePair('u', 'h') == std::vector<std::string>{"shang"}, "MS 'uh' -> 'shang'");

        // Semicolon ';' for 'ing'
        auto ing = ms.DecodePair('y', ';');
        AssertCondition(ing == std::vector<std::string>{"ying"}, "MS 'y;' -> 'ying'");
    }

    // 3. Xiaohe Shuangpin (小鹤双拼)
    {
        ShuangpinDecoder xh(ShuangpinLayout::Xiaohe);
        // Zero-initials
        AssertCondition(xh.DecodePair('a', 'a') == std::vector<std::string>{"a"}, "Xiaohe zero-initial 'aa' -> 'a'");
        AssertCondition(xh.DecodePair('a', 'i') == std::vector<std::string>{"ai"}, "Xiaohe zero-initial 'ai' -> 'ai'");
        AssertCondition(xh.DecodePair('a', 'h') == std::vector<std::string>{"ang"}, "Xiaohe zero-initial 'ah' -> 'ang'");

        // Distinct Xiaohe key mappings:
        // 'd' = 'ai', 'c' = 'ao', 'w' = 'ei', 'z' = 'ou', 'm' = 'ian', 'n' = 'iao'
        AssertCondition(xh.DecodePair('l', 'd') == std::vector<std::string>{"lai"}, "Xiaohe 'ld' -> 'lai'");
        AssertCondition(xh.DecodePair('b', 'c') == std::vector<std::string>{"bao"}, "Xiaohe 'bc' -> 'bao'");
        AssertCondition(xh.DecodePair('d', 'w') == std::vector<std::string>{"dei"}, "Xiaohe 'dw' -> 'dei'");
        AssertCondition(xh.DecodePair('z', 'z') == std::vector<std::string>{"zou"}, "Xiaohe 'zz' -> 'zou'");
        AssertCondition(xh.DecodePair('x', 'm') == std::vector<std::string>{"xian"}, "Xiaohe 'xm' -> 'xian'");
        AssertCondition(xh.DecodePair('n', 'n') == std::vector<std::string>{"niao"}, "Xiaohe 'nn' -> 'niao'");
        AssertCondition(xh.DecodePair('v', 's') == std::vector<std::string>{"zhong"}, "Xiaohe 'vs' -> 'zhong'");
    }

    std::cout << "  [PASS] All Shuangpin standard test vectors passed cleanly!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: Input Method Order & Mixed Input Isolation (Contract 1)
// -----------------------------------------------------------------------------
static void TestInputMethodAndMixedIsolation(InputCoordinator& coordinator) {
    std::cout << "[TEST 2] Testing Input Method Priority & Mixed Input Isolation..." << std::endl;

    // A. Mixed Input Disabled (Default: Wubi86 only)
    InputPolicy policyWubi;
    policyWubi.primary_method = InputScheme::Wubi86;
    policyWubi.mixed_input_enabled = false;
    coordinator.SetPolicy(policyWubi);

    auto resWubi = coordinator.Query("aaaa");
    AssertCondition(resWubi.status == CoordinatorStatus::Success, "Wubi query successful");
    AssertCondition(!resWubi.candidates.empty() && resWubi.candidates[0].text == L"工", "Wubi matched '工'");

    auto resPyDisabled = coordinator.Query("zhongwen");
    AssertCondition(resPyDisabled.status == CoordinatorStatus::InvalidCode || resPyDisabled.status == CoordinatorStatus::NoCandidates,
                    "Pinyin disabled when mixed input is off in Wubi mode");

    // B. Mixed Input Enabled (Wubi86 primary, fallback to Pinyin and Shuangpin)
    InputPolicy policyMixedWubi;
    policyMixedWubi.primary_method = InputScheme::Wubi86;
    policyMixedWubi.mixed_input_enabled = true;
    policyMixedWubi.input_method_order = {InputScheme::Wubi86, InputScheme::Pinyin, InputScheme::Shuangpin};
    policyMixedWubi.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_WUBI, true},
        {DICT_ID_BUILTIN_PINYIN, true}
    };
    coordinator.SetPolicy(policyMixedWubi);

    // 1. Primary Wubi: 'aaaa' matches Wubi '工' in dict_builtin_wubi
    auto resWubiMixed = coordinator.Query("aaaa");
    AssertCondition(resWubiMixed.status == CoordinatorStatus::Success, "Query 'aaaa' success");
    AssertCondition(!resWubiMixed.candidates.empty() && resWubiMixed.candidates[0].text == L"工", "Wubi matched '工'");
    AssertCondition(resWubiMixed.active_dict_id == DICT_ID_BUILTIN_WUBI, "Active dict is builtin wubi");

    // 2. Pinyin fallback: 'zhongwen' falls back to Pinyin -> '中文'
    auto resPyFallback = coordinator.Query("zhongwen");
    AssertCondition(resPyFallback.status == CoordinatorStatus::Success, "Mixed fallback to pinyin 'zhongwen' successful");
    AssertCondition(!resPyFallback.candidates.empty() && resPyFallback.candidates[0].text == L"中文", "Pinyin fallback matched '中文'");
    AssertCondition(resPyFallback.active_dict_id == DICT_ID_BUILTIN_PINYIN, "Active dict is builtin pinyin");

    // 3. Shuangpin fallback: 'xdxd' -> '想想'/'想象'
    auto resSpFallback = coordinator.Query("xdxd");
    AssertCondition(resSpFallback.status == CoordinatorStatus::Success, "Mixed fallback to shuangpin 'xdxd' successful");
    AssertCondition(!resSpFallback.candidates.empty(), "Shuangpin candidates returned for 'xdxd'");
    AssertCondition(resSpFallback.candidates[0].scheme == InputScheme::Shuangpin, "Candidate scheme is Shuangpin");
    AssertCondition(resSpFallback.candidates[0].layout_id == "natural", "Candidate layout is natural");

    std::cout << "  [PASS] Input method priority and mixed input isolation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 3: Cascading Fallback & Exact Match Over Prefix Completion
// -----------------------------------------------------------------------------
static void TestCascadingFallbackAndExactOverPrefix(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 3] Testing Cascading Fallback & Exact Match Over Prefix Completion..." << std::endl;

    std::string customId = repo.AddEntry(L"测试补全", {{InputScheme::Wubi86, "abcd"}});
    AssertCondition(!customId.empty(), "Custom prefix entry added");

    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.mixed_input_enabled = false;
    policy.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_WUBI, true}
    };
    coordinator.SetPolicy(policy);

    auto qA = coordinator.Query("a");
    AssertCondition(qA.status == CoordinatorStatus::Success, "Query 'a' success");
    AssertCondition(!qA.candidates.empty(), "Query 'a' returned candidates");
    AssertCondition(qA.candidates[0].quality == MatchQuality::ExactFull, "Candidate 0 is ExactFull");

    repo.DeleteEntry(customId);
    std::cout << "  [PASS] Exact match over prefix completion verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 4: Manual Fallback & Skipping Empty Middle Dictionaries (P2-1)
// -----------------------------------------------------------------------------
static void TestManualDictionaryNavigationWithEmptyMiddle(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 4] Testing Manual Dictionary Fallback & Skipping Empty Dictionaries (P2-1)..." << std::endl;

    std::string customId = repo.AddEntry(L"自定义工工工", {{InputScheme::Wubi86, "aaaa"}});

    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.mixed_input_enabled = false;
    // Order: Custom (has 'aaaa') -> Builtin Pinyin (0 matches for 'aaaa') -> Builtin Wubi (has 'aaaa')
    policy.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_PINYIN, true},
        {DICT_ID_BUILTIN_WUBI, true}
    };
    coordinator.SetPolicy(policy);

    auto res1 = coordinator.Query("aaaa");
    AssertCondition(res1.status == CoordinatorStatus::Success, "Query aaaa success");
    AssertCondition(res1.active_dict_id == DICT_ID_CUSTOM, "Active dict is custom");
    AssertCondition(res1.has_next_dictionary == true, "has_next_dictionary must be true");

    // Call QueryNextDictionary: must SKIP DICT_ID_BUILTIN_PINYIN (0 candidates) and return DICT_ID_BUILTIN_WUBI!
    auto resNext = coordinator.QueryNextDictionary("aaaa", res1.current_dict_index);
    AssertCondition(resNext.status == CoordinatorStatus::Success, "QueryNextDictionary success");
    AssertCondition(resNext.active_dict_id == DICT_ID_BUILTIN_WUBI, "Active dict jumped to builtin wubi, skipping empty pinyin dict");
    AssertCondition(resNext.candidates.size() >= 1, "Builtin wubi returned candidates");
    AssertCondition(resNext.candidates[0].text == L"工", "Builtin matches '工'");

    repo.DeleteEntry(customId);
    std::cout << "  [PASS] Manual dictionary navigation skipping empty middle dictionaries verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 5: P1-1 Learning Switch & 4-Combination Matrix & Idempotent Replay
// -----------------------------------------------------------------------------
static void TestLearningDisabledAndCommitMatrix(InputCoordinator& coordinator, EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 5] Testing P1-1 Learning Switch & 4-Combination Matrix & Idempotent Replay..." << std::endl;

    std::string testId = repo.AddEntry(L"测试学习开关", {{InputScheme::Wubi86, "aaaa"}});
    AssertCondition(!testId.empty(), "Added test entry");

    // Combination 1: freq=true, learn=false (P1-1 Review Probe assertion)
    {
        InputPolicy p;
        p.primary_method = InputScheme::Wubi86;
        p.dictionary_order = {{DICT_ID_CUSTOM, true}};
        p.frequency_enabled = true;
        p.learning_enabled = false;
        coordinator.SetPolicy(p);

        auto q = coordinator.Query("aaaa");
        AssertCondition(!q.candidates.empty(), "Candidate found");
        bool ok = coordinator.CommitCandidate(q.candidates[0], "test-event-learn-off-1", 100, 200);
        AssertCondition(ok, "CommitCandidate returned ACK=true even with learning_enabled=false");

        // Verify entry_stats was NOT created/incremented
        auto stat = repo.GetStat(InputScheme::Wubi86, "aaaa", testId);
        AssertCondition(!stat.has_value() || stat->selected_count == 0, "Stats NOT incremented when learning is false");

        // Verify commit_events was durably recorded for Stage 3 recovery/audit
        auto ce = repo.GetCommitEventRecord("test-event-learn-off-1");
        AssertCondition(ce.status == 1 && ce.session_id == 100 && ce.context_id == 200, "Commit event durably recorded");

        // Idempotent retry of identical event
        bool replayOk = coordinator.CommitCandidate(q.candidates[0], "test-event-learn-off-1", 100, 200);
        AssertCondition(replayOk, "Idempotent replay succeeds");
    }

    // Combination 2: freq=true, learn=true
    {
        InputPolicy p;
        p.primary_method = InputScheme::Wubi86;
        p.dictionary_order = {{DICT_ID_CUSTOM, true}};
        p.frequency_enabled = true;
        p.learning_enabled = true;
        coordinator.SetPolicy(p);

        auto q = coordinator.Query("aaaa");
        bool ok = coordinator.CommitCandidate(q.candidates[0], "test-event-learn-on-1", 100, 200);
        AssertCondition(ok, "CommitCandidate returned true");

        auto stat = repo.GetStat(InputScheme::Wubi86, "aaaa", testId);
        AssertCondition(stat.has_value() && stat->selected_count == 1, "Stats incremented to 1 when learning is true");

        // Idempotent replay: must not double-increment stats
        bool replayOk = coordinator.CommitCandidate(q.candidates[0], "test-event-learn-on-1", 100, 200);
        AssertCondition(replayOk, "Idempotent replay succeeds");
        auto statReplay = repo.GetStat(InputScheme::Wubi86, "aaaa", testId);
        AssertCondition(statReplay.has_value() && statReplay->selected_count == 1, "Stats NOT double incremented on replay");
    }

    // Combination 3: freq=false, learn=true
    {
        InputPolicy p;
        p.primary_method = InputScheme::Wubi86;
        p.dictionary_order = {{DICT_ID_CUSTOM, true}};
        p.frequency_enabled = false;
        p.learning_enabled = true;
        coordinator.SetPolicy(p);

        auto q = coordinator.Query("aaaa");
        // candidate selected_count and effective_count must be frozen to 0 in query results
        AssertCondition(q.candidates[0].selected_count == 0, "Frequency frozen in query results when frequency_enabled=false");

        bool ok = coordinator.CommitCandidate(q.candidates[0], "test-event-learn-on-freq-off", 100, 200);
        AssertCondition(ok, "CommitCandidate returned true");
        auto stat = repo.GetStat(InputScheme::Wubi86, "aaaa", testId);
        AssertCondition(stat.has_value() && stat->selected_count == 2, "Stats updated in DB even when frequency influence is frozen");
    }

    // Combination 4: freq=false, learn=false
    {
        InputPolicy p;
        p.primary_method = InputScheme::Wubi86;
        p.dictionary_order = {{DICT_ID_CUSTOM, true}};
        p.frequency_enabled = false;
        p.learning_enabled = false;
        coordinator.SetPolicy(p);

        auto q = coordinator.Query("aaaa");
        bool ok = coordinator.CommitCandidate(q.candidates[0], "test-event-both-off", 100, 200);
        AssertCondition(ok, "CommitCandidate returned true");
        auto stat = repo.GetStat(InputScheme::Wubi86, "aaaa", testId);
        AssertCondition(stat.has_value() && stat->selected_count == 2, "Stats NOT modified when learning_enabled=false");
    }

    repo.DeleteEntry(testId);
    std::cout << "  [PASS] Learning switch 4-combination matrix & idempotency verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 6: P1-2 & P1-3 Shuangpin Custom & Builtin Commits & Builtin Stats Loading
// -----------------------------------------------------------------------------
static void TestShuangpinAndBuiltinCommits(InputCoordinator& coordinator, EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 6] Testing Shuangpin Custom & Builtin Commits & Builtin Stats Loading..." << std::endl;

    // 1. Shuangpin Custom Candidate Commit (P1-2)
    std::string pyId = repo.AddEntry(L"双拼自定义测试词", {{InputScheme::Pinyin, "zhongwen"}});
    AssertCondition(!pyId.empty(), "Added custom pinyin entry");

    InputPolicy policySp;
    policySp.primary_method = InputScheme::Shuangpin;
    policySp.shuangpin_layout = ShuangpinLayout::Natural;
    policySp.learning_enabled = true;
    policySp.dictionary_order = {{DICT_ID_CUSTOM, true}};
    coordinator.SetPolicy(policySp);

    auto qSp = coordinator.Query("vswf");
    AssertCondition(!qSp.candidates.empty(), "Query Natural vswf found custom candidate");
    AssertCondition(qSp.candidates[0].entry_id == pyId, "Found expected candidate ID");

    bool spOk = coordinator.CommitCandidate(qSp.candidates[0], "event-sp-commit-1", 1, 1);
    AssertCondition(spOk, "Shuangpin custom candidate commit returned ACK=true (P1-2 fixed)");

    // Verify stats recorded under scoped code "natural:vswf"
    auto spStat = repo.GetStat(InputScheme::Shuangpin, "natural:vswf", pyId);
    AssertCondition(spStat.has_value() && spStat->selected_count == 1, "Stats recorded under scoped code 'natural:vswf'");

    // 2. First-time Builtin Candidate Commit in Clean DB (P1-2 clean DB builtin commit)
    InputPolicy policyWubi;
    policyWubi.primary_method = InputScheme::Wubi86;
    policyWubi.dictionary_order = {{DICT_ID_BUILTIN_WUBI, true}};
    policyWubi.learning_enabled = true;
    coordinator.SetPolicy(policyWubi);

    auto qWubi = coordinator.Query("aaaa");
    AssertCondition(!qWubi.candidates.empty(), "Query aaaa found builtin candidate");
    auto bc = qWubi.candidates[0];
    AssertCondition(bc.source == EntrySource::Builtin, "Candidate is builtin");

    bool builtinOk = coordinator.CommitCandidate(bc, "event-builtin-clean-db-1", 1, 1);
    AssertCondition(builtinOk, "Builtin candidate commit in clean DB returned ACK=true (P1-2 fixed)");

    // 3. Builtin candidate stats reflection during query (P1-3)
    int64_t now = clock->Now();
    repo.SetStatDirect(InputScheme::Wubi86, "aaaa", bc.entry_id, 100, now, 100.0, now);
    auto qWubiReload = coordinator.Query("aaaa");
    bool found100 = false;
    for (const auto& c : qWubiReload.candidates) {
        if (c.entry_id == bc.entry_id) {
            AssertCondition(c.selected_count == 100, "Builtin candidate loaded personal stat count 100 (P1-3 fixed)");
            found100 = true;
            break;
        }
    }
    AssertCondition(found100, "Builtin candidate with count 100 verified in query");

    repo.DeleteEntry(pyId);
    std::cout << "  [PASS] Shuangpin custom & builtin clean DB commits and builtin stats loading verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 7: P1-4 Pin & Stats Scope Isolation (Pinyin to Shuangpin & Layouts)
// -----------------------------------------------------------------------------
static void TestPinAndStatsScopeIsolation(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 7] Testing P1-4 Pin & Stats Scope Isolation..." << std::endl;

    std::string pyId = repo.AddEntry(L"全拼置顶词", {{InputScheme::Pinyin, "zhongwen"}});

    // Set pin preference ONLY in Pinyin scheme under "zhongwen"
    bool setPin = repo.SetPinPreference(InputScheme::Pinyin, "zhongwen", pyId, true, 1);
    AssertCondition(setPin, "SetPinPreference on Pinyin zhongwen succeeded");

    // Query in Shuangpin mode: pin preference MUST NOT leak into Shuangpin!
    InputPolicy pSp;
    pSp.primary_method = InputScheme::Shuangpin;
    pSp.shuangpin_layout = ShuangpinLayout::Natural;
    pSp.dictionary_order = {{DICT_ID_CUSTOM, true}};
    coordinator.SetPolicy(pSp);

    auto qSp = coordinator.Query("vswf");
    AssertCondition(!qSp.candidates.empty(), "Candidate found in Shuangpin");
    AssertCondition(qSp.candidates[0].is_pinned == false,
                    "Pinyin pin preference must NOT leak into Shuangpin query (P1-4 fixed)");

    // Now pin explicitly in Shuangpin Natural:
    coordinator.SetPinPreference(InputScheme::Shuangpin, "vswf", pyId, true, 1, "natural");
    auto qSpNat = coordinator.Query("vswf");
    AssertCondition(qSpNat.candidates[0].is_pinned == true, "Natural pin preference active in Natural");

    // Switch to Xiaohe: pin MUST NOT leak to Xiaohe
    pSp.shuangpin_layout = ShuangpinLayout::Xiaohe;
    coordinator.SetPolicy(pSp);
    auto qSpXh = coordinator.Query("vswf");
    if (!qSpXh.candidates.empty()) {
        AssertCondition(qSpXh.candidates[0].is_pinned == false, "Natural pin preference must NOT leak to Xiaohe layout");
    }

    coordinator.UnpinPreference(InputScheme::Shuangpin, "vswf", pyId, "natural");
    repo.UnpinPreference(InputScheme::Pinyin, "zhongwen", pyId);
    repo.DeleteEntry(pyId);
    std::cout << "  [PASS] Pin & stats scope isolation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 8: P1-6 Shuangpin Trailing Incomplete & Illegal Input Validation
// -----------------------------------------------------------------------------
static void TestShuangpinIncompleteAndIllegalValidation(InputCoordinator& coordinator) {
    std::cout << "[TEST 8] Testing P1-6 Shuangpin Incomplete & Illegal Input Validation..." << std::endl;

    InputPolicy p;
    p.primary_method = InputScheme::Shuangpin;
    p.shuangpin_layout = ShuangpinLayout::Natural;
    p.dictionary_order = {{DICT_ID_BUILTIN_PINYIN, true}};
    p.mixed_input_enabled = false;
    coordinator.SetPolicy(p);

    ShuangpinDecoder dec(ShuangpinLayout::Natural);

    // 1. "vs" -> complete, zhong, count 46
    auto dVs = dec.Decode("vs");
    AssertCondition(dVs.is_valid && dVs.is_complete && dVs.pinyin_continuous == "zhong", "vs decode ok");
    auto rVs = coordinator.Query("vs");
    AssertCondition(rVs.status == CoordinatorStatus::Success && rVs.candidates.size() == 46, "vs count is 46");

    // 2. "vsw" -> valid, incomplete trailing 'w', continuous 'zhongw', count < 46
    auto dVsw = dec.Decode("vsw");
    AssertCondition(dVsw.is_valid && !dVsw.is_complete && dVsw.trailing_incomplete == "w", "vsw incomplete ok");
    AssertCondition(dVsw.pinyin_continuous == "zhongw", "vsw resolves to zhongw");
    auto rVsw = coordinator.Query("vsw");
    AssertCondition(rVsw.status == CoordinatorStatus::Success && rVsw.candidates.size() < 46 && rVsw.candidates.size() >= 1,
                    "vsw does NOT drop 'w' and filters candidates matching zhongw (P1-6 fixed)");

    // 3. "vs1" -> illegal digit '1', is_valid = false, query returns InvalidCode
    auto dVs1 = dec.Decode("vs1");
    AssertCondition(!dVs1.is_valid, "vs1 is_valid is false (rejects non-letter)");
    auto rVs1 = coordinator.Query("vs1");
    AssertCondition(rVs1.status == CoordinatorStatus::InvalidCode, "vs1 query returns InvalidCode (P1-6 fixed)");

    // 4. "1" -> illegal character, is_valid = false, query returns InvalidCode
    auto d1 = dec.Decode("1");
    AssertCondition(!d1.is_valid, "1 is_valid is false");
    auto r1 = coordinator.Query("1");
    AssertCondition(r1.status == CoordinatorStatus::InvalidCode, "1 query returns InvalidCode (P1-6 fixed)");

    // 5. "vs'wf" -> valid separator, resolves to zhongwen
    auto dSepOk = dec.Decode("vs'wf");
    AssertCondition(dSepOk.is_valid && dSepOk.is_complete && dSepOk.pinyin_continuous == "zhongwen", "vs'wf valid");
    auto rSepOk = coordinator.Query("vs'wf");
    AssertCondition(rSepOk.status == CoordinatorStatus::Success, "vs'wf query ok");

    // 6. "v'swf" -> invalid separator placement (after odd length chunk), is_valid = false
    auto dSepBad = dec.Decode("v'swf");
    AssertCondition(!dSepBad.is_valid, "v'swf is_valid is false");
    auto rSepBad = coordinator.Query("v'swf");
    AssertCondition(rSepBad.status == CoordinatorStatus::InvalidCode, "v'swf query returns InvalidCode");

    std::cout << "  [PASS] Shuangpin incomplete & illegal character validation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 9: P1-5 Storage Error Distinction (No Silent Fallback on DB Corruption)
// -----------------------------------------------------------------------------
static void TestStorageErrorDistinction(BuiltinDictionary& builtinDict, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 9] Testing P1-5 Storage Error Distinction..." << std::endl;

    SqliteDb brokenDb;
    bool openOk = brokenDb.Open(":memory:");
    AssertCondition(openOk && Migration::Migrate(brokenDb), "Memory DB initialized");

    EntryRepository brokenRepo(brokenDb, clock);
    InputCoordinator brokenCoord(brokenRepo, builtinDict, clock);

    // Normal query with valid DB returns candidates
    InputPolicy p;
    p.primary_method = InputScheme::Wubi86;
    p.dictionary_order = {{DICT_ID_CUSTOM, true}, {DICT_ID_BUILTIN_WUBI, true}};
    brokenCoord.SetPolicy(p);

    auto qNormal = brokenCoord.Query("aaaa");
    AssertCondition(qNormal.status == CoordinatorStatus::Success, "Normal query before table drop ok");

    // Intentionally DROP table entry_codes to simulate storage failure
    brokenDb.Execute("DROP TABLE entry_codes;");

    // Query must return CoordinatorStatus::StorageError, NOT silently fall back to builtin wubi!
    auto qBroken = brokenCoord.Query("aaaa");
    AssertCondition(qBroken.status == CoordinatorStatus::StorageError, "Broken DB returns StorageError (P1-5 fixed)");
    AssertCondition(qBroken.candidates.empty(), "Candidates cleared on storage error");
    AssertCondition(qBroken.active_dict_id.empty(), "Active dict empty on storage error");

    brokenDb.Close();
    std::cout << "  [PASS] Storage error distinction without silent fallback verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 10: P2-2 Input Method Priority in Mixed Ranking
// -----------------------------------------------------------------------------
static void TestInputMethodPriorityInRanking(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 10] Testing P2-2 Input Method Priority in Mixed Ranking..." << std::endl;

    // Add a custom Wubi entry (priority 0) and custom Pinyin entry (priority 1) for the same code 'ba'
    std::string wbId = repo.AddEntry(L"五笔候选节", {{InputScheme::Wubi86, "ba"}});
    std::string pyId = repo.AddEntry(L"拼音候选把柄", {{InputScheme::Pinyin, "ba"}});

    // Artificially give the Pinyin entry high frequency 100, while Wubi entry has 0
    repo.SetStatDirect(InputScheme::Pinyin, "ba", pyId, 100, 1000, 100.0, 1000);

    // Set mixed input with Wubi86 priority 0 and Pinyin priority 1
    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.mixed_input_enabled = true;
    policy.input_method_order = {InputScheme::Wubi86, InputScheme::Pinyin};
    policy.dictionary_order = {{DICT_ID_CUSTOM, true}};
    coordinator.SetPolicy(policy);

    // Query 'ba':
    // Both match ExactFull in DICT_ID_CUSTOM.
    auto q = coordinator.Query("ba");
    AssertCondition(q.status == CoordinatorStatus::Success, "Query 'ba' success");
    AssertCondition(q.candidates.size() >= 2, "Both Wubi and Pinyin candidates returned");

    // Even though Pinyin has high frequency (100), candidate 0 MUST be from method priority 0 (Wubi86)!
    AssertCondition(q.candidates[0].scheme == InputScheme::Wubi86,
                    "Candidate #0 is from primary method Wubi86 (P2-2 fixed)");
    AssertCondition(q.candidates[0].method_priority == 0, "Candidate #0 has method_priority 0");
    AssertCondition(q.candidates[0].text == L"五笔候选节", "Candidate #0 text is Wubi entry");

    AssertCondition(q.candidates[1].scheme == InputScheme::Pinyin, "Candidate #1 is from secondary method Pinyin");
    AssertCondition(q.candidates[1].method_priority == 1, "Candidate #1 has method_priority 1");
    AssertCondition(q.candidates[1].text == L"拼音候选把柄", "Candidate #1 text is Pinyin entry");

    repo.DeleteEntry(wbId);
    repo.DeleteEntry(pyId);
    std::cout << "  [PASS] Input method priority in mixed ranking verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 11: P1-7 Concurrency & Thread-Safety (Zero Data Race on Shared Decoders)
// -----------------------------------------------------------------------------
static void TestConcurrentQueryThreadSafety(InputCoordinator& coordinator) {
    std::cout << "[TEST 11] Testing P1-7 Concurrency & Thread-Safety (Shared Decoders)..." << std::endl;

    // Create 3 policies with different layouts
    auto pNat = std::make_shared<InputPolicy>();
    pNat->primary_method = InputScheme::Shuangpin;
    pNat->shuangpin_layout = ShuangpinLayout::Natural;

    auto pMs = std::make_shared<InputPolicy>();
    pMs->primary_method = InputScheme::Shuangpin;
    pMs->shuangpin_layout = ShuangpinLayout::Microsoft;

    auto pXh = std::make_shared<InputPolicy>();
    pXh->primary_method = InputScheme::Shuangpin;
    pXh->shuangpin_layout = ShuangpinLayout::Xiaohe;

    std::vector<std::future<void>> futures;
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS_PER_THREAD = 50;

    for (int t = 0; t < NUM_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&coordinator, pNat, pMs, pXh, t]() {
            for (int i = 0; i < ITERS_PER_THREAD; ++i) {
                if (t % 3 == 0) {
                    auto r = coordinator.Query("vswf", pNat);
                    AssertCondition(r.status == CoordinatorStatus::Success || r.status == CoordinatorStatus::NoCandidates, "Nat query ok");
                } else if (t % 3 == 1) {
                    auto r = coordinator.Query("vswf", pMs);
                    AssertCondition(r.status == CoordinatorStatus::Success || r.status == CoordinatorStatus::NoCandidates, "MS query ok");
                } else {
                    auto r = coordinator.Query("vswf", pXh);
                    AssertCondition(r.status == CoordinatorStatus::Success || r.status == CoordinatorStatus::NoCandidates, "Xiaohe query ok");
                }
            }
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    std::cout << "  [PASS] Concurrent query thread-safety across different policy layouts verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 12: P1-1 Recalled Candidates ExactOnly Filtering
// -----------------------------------------------------------------------------
static void TestReviewRound2P1_1RecallExactFilter(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 12] Testing P1-1 Recalled Candidates ExactOnly Filtering..." << std::endl;

    const std::string synthPath = "out/stage4a-review/synthetic_p1_1";
    std::filesystem::create_directories(synthPath);
    {
        std::ofstream f(synthPath + "/wubi86.dict");
        f << "Prefix\tabcd\t10\n";
    }
    {
        std::ofstream f(synthPath + "/pinyin.dict");
        f << "Dummy\ta\ta\t10\n";
    }

    BuiltinDictionary synthDict;
    AssertCondition(synthDict.Load(synthPath), "Loaded synthetic dictionary for P1-1");

    InputCoordinator synthCoord(repo, synthDict, clock);

    // 1. Add custom exact entry for 'ab'
    std::string custId = repo.AddEntry(L"ExactCustom", {{InputScheme::Wubi86, "ab"}});

    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.dictionary_order = {
        {DICT_ID_BUILTIN_WUBI, true},
        {DICT_ID_CUSTOM, true}
    };
    synthCoord.SetPolicy(policy);

    // Before recall: builtin wubi only has prefix completion 'abcd', custom has exact 'ab'
    auto qBefore = synthCoord.Query("ab");
    AssertCondition(qBefore.active_dict_id == DICT_ID_CUSTOM, "Before recall: selects custom");
    AssertCondition(qBefore.candidates[0].quality == MatchQuality::ExactFull, "Before recall: quality is ExactFull");

    // 2. Ensure builtin prefix item exists and pin it to query 'ab'
    std::string prefixId = "b_wb_abcd_Prefix";
    repo.EnsureBuiltinEntryExists(prefixId, L"Prefix");
    repo.SetPinPreference(InputScheme::Wubi86, "ab", prefixId, true, 1);

    // Query 'ab':
    // DICT_ID_BUILTIN_WUBI does not have an exact match for 'ab'.
    // Even though b_wb_abcd_Prefix is pinned for 'ab', it is a PrefixCompletion (quality != ExactFull).
    // In exactOnly stage, it must NOT enter exactCache.
    // Therefore, the exact stage must fall through to DICT_ID_CUSTOM and return 'ExactCustom' (quality == ExactFull).
    auto qAfter = synthCoord.Query("ab");
    AssertCondition(qAfter.status == CoordinatorStatus::Success, "Query 'ab' succeeds");
    AssertCondition(!qAfter.candidates.empty(), "Query 'ab' has candidates");
    AssertCondition(qAfter.active_dict_id == DICT_ID_CUSTOM, "Exact stage selects DICT_ID_CUSTOM despite pinned prefix in builtin (P1-1 fixed)");
    AssertCondition(qAfter.candidates[0].quality == MatchQuality::ExactFull, "Top candidate is ExactFull");
    AssertCondition(qAfter.candidates[0].text == L"ExactCustom", "Top candidate is custom exact match");

    // Clean up
    repo.UnpinPreference(InputScheme::Wubi86, "ab", prefixId);
    repo.DeleteEntry(custId);
    std::filesystem::remove_all(synthPath);
    std::cout << "  [PASS] Recalled prefix cannot bypass exactOnly filter verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 13: P1-2 Shuangpin Scoped Recall Key (Pin & Stats Outside Pool)
// -----------------------------------------------------------------------------
static void TestReviewRound2P1_2ShuangpinScopedRecall(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 13] Testing P1-2 Shuangpin Scoped Recall Key (Pin & Stats Outside Pool)..." << std::endl;

    const std::string synthPath = "out/stage4a-review/synthetic_p1_2";
    std::filesystem::create_directories(synthPath);
    {
        std::ofstream f(synthPath + "/pinyin.dict");
        for (int i = 0; i < 515; ++i) {
            f << "Fill" << i << "\tzhongwen\tzhong wen\t10\n";
        }
        f << "TargetOutside\tzhongwenzhong\tzhong wen zhong\t10\n";
    }
    {
        std::ofstream f(synthPath + "/wubi86.dict");
        f << "Dummy\ta\t10\n";
    }

    BuiltinDictionary synthDict;
    AssertCondition(synthDict.Load(synthPath), "Loaded synthetic dictionary for P1-2");

    InputCoordinator synthCoord(repo, synthDict, clock);

    InputPolicy policy;
    policy.primary_method = InputScheme::Shuangpin;
    policy.shuangpin_layout = ShuangpinLayout::Natural;
    policy.dictionary_order = {{DICT_ID_BUILTIN_PINYIN, true}};
    synthCoord.SetPolicy(policy);

    std::string targetId = "b_py_zhongwenzhong_TargetOutside";
    repo.EnsureBuiltinEntryExists(targetId, L"TargetOutside");

    // Pin target under Shuangpin with layout 'natural' on code 'vsw'
    bool pinOk = synthCoord.SetPinPreference(InputScheme::Shuangpin, "vsw", targetId, true, 1, "natural");
    AssertCondition(pinOk, "Pinned target under Shuangpin natural:vsw");

    // Query 'vsw'
    auto q = synthCoord.Query("vsw");
    AssertCondition(q.status == CoordinatorStatus::Success, "Query 'vsw' succeeded");
    AssertCondition(q.candidates.size() >= 513, "Candidate pool includes recalled item");

    bool targetFound = false;
    for (const auto& c : q.candidates) {
        if (c.entry_id == targetId) {
            targetFound = true;
            AssertCondition(c.is_pinned, "Target candidate is marked pinned");
            AssertCondition(c.order_rank == 1, "Target candidate order rank is 1");
            break;
        }
    }
    AssertCondition(targetFound, "Target outside bounded pool recalled via Shuangpin scoped key (P1-2 fixed)");

    // Also test active stats recall under Shuangpin
    synthCoord.UnpinPreference(InputScheme::Shuangpin, "vsw", targetId, "natural");
    std::string scopedKey = "natural:vsw";
    repo.SetStatDirect(InputScheme::Shuangpin, scopedKey, targetId, 50, 1000, 50.0, 1000);

    auto qStat = synthCoord.Query("vsw");
    bool statTargetFound = false;
    for (const auto& c : qStat.candidates) {
        if (c.entry_id == targetId) {
            statTargetFound = true;
            AssertCondition(c.selected_count == 50, "Target selected count matches scoped stat");
            break;
        }
    }
    AssertCondition(statTargetFound, "Target outside pool recalled via active stats under Shuangpin scoped key");

    std::filesystem::remove_all(synthPath);
    std::cout << "  [PASS] Shuangpin scoped recall key for pin and stats verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 14: P1-3 Shuangpin Syllable Boundaries
// -----------------------------------------------------------------------------
static void TestReviewRound2P1_3ShuangpinSyllableBoundaries(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 14] Testing P1-3 Shuangpin Syllable Boundaries (xi'an vs xian)..." << std::endl;

    const std::string synthPath = "out/stage4a-review/synthetic_p1_3";
    std::filesystem::create_directories(synthPath);
    {
        std::ofstream f(synthPath + "/pinyin.dict");
        f << "One\txian\txian\t10\n";
        f << "Two\txian\txi an\t10\n";
    }
    {
        std::ofstream f(synthPath + "/wubi86.dict");
        f << "Dummy\ta\t10\n";
    }

    BuiltinDictionary synthDict;
    AssertCondition(synthDict.Load(synthPath), "Loaded synthetic dictionary for P1-3");
    InputCoordinator synthCoord(repo, synthDict, clock);

    InputPolicy policy;
    policy.primary_method = InputScheme::Shuangpin;
    policy.shuangpin_layout = ShuangpinLayout::Natural;
    policy.dictionary_order = {{DICT_ID_BUILTIN_PINYIN, true}};
    synthCoord.SetPolicy(policy);

    // Query "xi'an": Natural shuangpin decodes to explicit syllables {"xi", "an"}
    auto q = synthCoord.Query("xi'an");
    AssertCondition(q.status == CoordinatorStatus::Success, "Query xi'an succeeded");

    bool hasOne = false;
    bool hasTwo = false;
    for (const auto& c : q.candidates) {
        if (c.text == L"One") hasOne = true;
        if (c.text == L"Two") hasTwo = true;
    }
    AssertCondition(!hasOne, "Explicit xi'an MUST NOT match single syllable xian ('One') (P1-3 fixed)");
    AssertCondition(hasTwo, "Explicit xi'an MUST match two-syllable xi an ('Two') (P1-3 fixed)");

    std::filesystem::remove_all(synthPath);
    std::cout << "  [PASS] Shuangpin explicit syllable boundary preservation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 15: P1-4 Bounded Combinatorial Decoding and Input Length Safeguards
// -----------------------------------------------------------------------------
static void TestReviewRound2P1_4BoundedCombinatorialDecoding() {
    std::cout << "[TEST 15] Testing P1-4 Bounded Combinatorial Decoding & Input Length Limit..." << std::endl;

    ShuangpinDecoder dec(ShuangpinLayout::Natural);

    // 1. Ambiguous input explosion test: 'nt' repeated 12 times (24 chars)
    // In unconstrained exponential expansion, this would produce 4096 branches.
    // With bounding, branches must be <= 16 and execution must take < 5ms.
    std::string ambiguous;
    for (int i = 0; i < 12; ++i) ambiguous += "nt";

    auto start = std::chrono::steady_clock::now();
    auto dAmb = dec.Decode(ambiguous);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    AssertCondition(dAmb.is_valid, "Ambiguous decode is valid");
    AssertCondition(dAmb.is_truncated, "Ambiguous decode is marked truncated");
    AssertCondition(dAmb.branches.size() <= MAX_SHUANGPIN_BRANCHES, "Branches capped at MAX_SHUANGPIN_BRANCHES (16)");
    AssertCondition(elapsedMs < 5, "Decode completed well within bounded time (< 5ms)");

    // 2. Input length upper bound: length > 64 must be rejected safely
    std::string longInput(65, 'a');
    auto dLong = dec.Decode(longInput);
    AssertCondition(!dLong.is_valid, "Input exceeding MAX_SHUANGPIN_INPUT_LENGTH (64) is rejected");
    AssertCondition(!dLong.error_detail.empty(), "Error detail describes length limit");

    // Exactly 64 chars
    std::string exactLimitInput(64, 'a'); // 32 'aa' pairs -> 'a a a ...'
    auto dLimit = dec.Decode(exactLimitInput);
    AssertCondition(dLimit.is_valid, "Input of exactly 64 chars is accepted");

    std::cout << "  [PASS] Bounded combinatorial decoding and length limits verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 16: Round 3 P1-1 Custom Dictionary Syllable Matching & Short-Text/Symbols
// -----------------------------------------------------------------------------
static void TestReviewRound3CustomSyllablesAndShortText(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 16] Testing Round 3 P1-1 Custom Syllables & Short-Text/Symbols..." << std::endl;

    BuiltinDictionary emptyDict;
    InputCoordinator co(repo, emptyDict, clock);
    InputPolicy p;
    p.primary_method = InputScheme::Shuangpin;
    p.dictionary_order = {{DICT_ID_CUSTOM, true}};
    co.SetPolicy(p);

    // 1. Long text with unseparated code 'xian': MUST NOT match 'xi'an'
    auto wrongId = repo.AddEntry(L"任意长短语", {{InputScheme::Pinyin, "xian"}});
    auto qXiAn = co.Query("xi'an");
    bool foundWrong = false;
    for (const auto& c : qXiAn.candidates) {
        if (c.entry_id == wrongId) foundWrong = true;
    }
    AssertCondition(!foundWrong, "xi'an query MUST NOT return unseparated 'xian' custom entry (P1-1 fixed)");

    // 2. Custom entry registered with explicit separator 'xi'an': MUST be found on xi'an query
    auto explicitId = repo.AddEntry(L"分隔编码", {{InputScheme::Pinyin, "xi'an"}});
    auto qExplicit = co.Query("xi'an");
    bool foundExplicit = false;
    for (const auto& c : qExplicit.candidates) {
        if (c.entry_id == explicitId) foundExplicit = true;
    }
    AssertCondition(foundExplicit, "xi'an query MUST return custom entry registered with 'xi'an' (P1-1 fixed)");

    // 3. Short text / symbol '★' with code 'zhongwen': MUST be found on Natural Shuangpin 'vswf'
    auto symbolId = repo.AddEntry(L"★", {{InputScheme::Pinyin, "zhongwen"}});
    auto qVswf = co.Query("vswf");
    bool foundSymbol = false;
    for (const auto& c : qVswf.candidates) {
        if (c.entry_id == symbolId) foundSymbol = true;
    }
    AssertCondition(foundSymbol, "vswf query MUST return short-text/symbol entry '★' without length heuristic (P1-1 fixed)");

    // 4. Commit candidate with explicit separator code: affiliation verification must succeed
    bool commitOk = co.CommitCandidate(qExplicit.candidates[0], "evt_r3_explicit", 1, 1);
    AssertCondition(commitOk, "CommitCandidate with explicit separator code 'xi'an' must succeed");

    repo.DeleteEntry(wrongId);
    repo.DeleteEntry(explicitId);
    repo.DeleteEntry(symbolId);
    std::cout << "  [PASS] Custom dictionary syllable boundaries, short-text and affiliation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 17: Round 3 P1-2 & P2 Truncation Propagation & Upper Bound Precision
// -----------------------------------------------------------------------------
static void TestReviewRound3TruncationPropagationAndBoundaries(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 17] Testing Round 3 P1-2 & P2 Truncation Propagation & Exact 16 Bounds..." << std::endl;

    ShuangpinDecoder dec(ShuangpinLayout::Natural);

    // 1. Exactly 16 branches (4 'nt' pairs = 2^4 = 16): MUST NOT be marked truncated (P2 fixed)
    std::string raw16;
    for (int i = 0; i < 4; ++i) raw16 += "nt";
    auto d16 = dec.Decode(raw16);
    AssertCondition(d16.branches.size() == 16, "4 'nt' pairs produces exactly 16 branches");
    AssertCondition(!d16.is_truncated, "Exactly 16 branches MUST NOT be marked truncated (P2 fixed)");

    // 2. 32 branches (5 'nt' pairs = 2^5 = 32): MUST be capped at 16 and marked truncated
    std::string raw32, code32;
    for (int i = 0; i < 5; ++i) {
        raw32 += "nt";
        code32 += "nve";
    }
    auto d32 = dec.Decode(raw32);
    AssertCondition(d32.branches.size() == 16, "5 'nt' pairs branches capped at 16");
    AssertCondition(d32.is_truncated, "5 'nt' pairs MUST be marked truncated");

    // 3. Coordinator query when target is in truncated search space:
    // Coordinator status MUST be BudgetTruncated (5), NOT generic NoCandidates (1) (P1-2 fixed)
    auto targetId = repo.AddEntry(L"后半解析测试词", {{InputScheme::Pinyin, code32}});
    BuiltinDictionary emptyDict;
    InputCoordinator co(repo, emptyDict, clock);
    InputPolicy p;
    p.primary_method = InputScheme::Shuangpin;
    p.dictionary_order = {{DICT_ID_CUSTOM, true}};
    co.SetPolicy(p);

    auto qTruncated = co.Query(raw32);
    AssertCondition(qTruncated.candidates.empty(), "Target in branch 32 not found in first 16 branches");
    AssertCondition(qTruncated.is_truncated, "CoordinatorQueryResult marked truncated (P1-2 fixed)");
    AssertCondition(qTruncated.status == CoordinatorStatus::BudgetTruncated, "Coordinator status is BudgetTruncated (5) (P1-2 fixed)");
    AssertCondition(!qTruncated.truncation_reason.empty(), "Truncation reason is provided");

    // 4. Input exceeding length 64: status MUST be InputLengthExceeded (6)
    std::string tooLong(65, 'a');
    auto qTooLong = co.Query(tooLong);
    AssertCondition(qTooLong.status == CoordinatorStatus::InputLengthExceeded, "Status is InputLengthExceeded (6)");

    repo.DeleteEntry(targetId);
    std::cout << "  [PASS] Truncation propagation, exact-16 bounds and length limit status verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 18: Round 4 Regressions (Prefix, Multi-code, Reverse-prefix, Numeric)
// -----------------------------------------------------------------------------
static void TestReviewRound4Regressions(EntryRepository& repo, std::shared_ptr<IClock> clock) {
    std::cout << "[TEST 18] Testing Round 4 Regressions (P1-1 Prefix, P1-2 Multi-code, P1-3 Reverse Prefix, P2 Numeric)..." << std::endl;

    BuiltinDictionary emptyDict;
    InputCoordinator co(repo, emptyDict, clock);
    InputPolicy p;
    p.primary_method = InputScheme::Pinyin;
    p.dictionary_order = {{DICT_ID_CUSTOM, true}};
    co.SetPolicy(p);

    // 1. P1-1: Incomplete Pinyin prefix queries must find multi-syllable custom entries
    auto zwId = repo.AddEntry(L"中文", {{InputScheme::Pinyin, "zhongwen"}});
    for (const auto& raw : {"z", "zhongw", "zhongwen"}) {
        auto q = co.Query(raw);
        bool found = false;
        for (const auto& c : q.candidates) {
            if (c.entry_id == zwId) found = true;
        }
        AssertCondition(found, std::string("Pinyin prefix query '") + raw + "' MUST find custom entry '中文' (P1-1 fixed)");
        AssertCondition(q.status == CoordinatorStatus::Success, std::string("Query '") + raw + "' status is Success");
    }

    // 2. P1-2: Multi-code entries with both single and multi-syllable spellings ('xian' and 'xi'an')
    auto multiId = repo.AddEntry(L"多编码", {{InputScheme::Pinyin, "xian"}, {InputScheme::Pinyin, "xi'an"}});
    p.primary_method = InputScheme::Shuangpin;
    p.shuangpin_layout = ShuangpinLayout::Natural;
    co.SetPolicy(p);

    for (const auto& raw : {"xian", "xi'an"}) {
        auto q = co.Query(raw);
        bool found = false;
        for (const auto& c : q.candidates) {
            if (c.entry_id == multiId) found = true;
        }
        AssertCondition(found, std::string("Shuangpin query '") + raw + "' MUST find multi-code entry '多编码' (P1-2 fixed)");
        AssertCondition(q.status == CoordinatorStatus::Success, std::string("Query '") + raw + "' status is Success");
    }

    // 3. P2: Pure numeric input in Pinyin scheme must return InvalidCode (4)
    p.primary_method = InputScheme::Pinyin;
    co.SetPolicy(p);
    auto qNum = co.Query("123");
    AssertCondition(qNum.status == CoordinatorStatus::InvalidCode, "Pinyin query '123' status MUST be InvalidCode (4) (P2 fixed)");
    AssertCondition(qNum.candidates.empty(), "Pinyin query '123' must return 0 candidates");

    // 4. P1-3: Reverse prefix affiliation verification must be rejected
    auto shortId = repo.AddEntry(L"短码", {{InputScheme::Wubi86, "a"}});
    bool reverseAff = repo.VerifyCandidateAffiliation(InputScheme::Wubi86, "abcd", shortId);
    AssertCondition(!reverseAff, "VerifyCandidateAffiliation MUST reject query 'abcd' for entry registered with short code 'a' (P1-3 fixed)");
    auto shortCands = repo.QueryUserCandidates(InputScheme::Wubi86, "abcd");
    AssertCondition(shortCands.empty(), "QueryUserCandidates('abcd') must not return entry registered only with 'a'");

    // Cleanup
    repo.DeleteEntry(zwId);
    repo.DeleteEntry(multiId);
    repo.DeleteEntry(shortId);
    std::cout << "  [PASS] Round 4 prefix completion, multi-code deduplication, reverse prefix and numeric rejection verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Finalization: completed syllables can also be continuous-pinyin prefixes;
// explicit boundaries must survive normal builtin lookup and recalled items.
static void TestFinalizationPinyinPrefixesAndBoundaries() {
    std::cout << "[TEST 19] Testing finalization pinyin prefixes and explicit recall boundaries..." << std::endl;
    SqliteDb db;
    AssertCondition(db.Open(":memory:") && Migration::Migrate(db), "Finalization isolated DB initialized");
    auto clock = std::make_shared<SystemClock>();
    EntryRepository repo(db, clock);
    BuiltinDictionary dict;
    InputCoordinator co(repo, dict, clock);
    InputPolicy policy;
    policy.primary_method = InputScheme::Pinyin;
    policy.dictionary_order = {{DICT_ID_CUSTOM, true}};
    co.SetPolicy(policy);
    auto idAn = repo.AddEntry(L"按", {{InputScheme::Pinyin, "an"}});
    auto idXian = repo.AddEntry(L"先", {{InputScheme::Pinyin, "xian"}});
    for (const auto& input : {"a", "xi", "xia"}) {
        auto q = co.Query(input);
        auto target = std::string(input) == "a" ? idAn : idXian;
        auto it = std::find_if(q.candidates.begin(), q.candidates.end(),
            [&](const Candidate& c) { return c.entry_id == target; });
        AssertCondition(it != q.candidates.end() && it->quality == MatchQuality::PrefixCompletion,
            std::string("Valid syllable remains a completion prefix: ") + input);
        AssertCondition(co.CommitCandidate(*it, std::string("final-prefix-") + input, 55, 66),
            "Prefix candidate can commit using its actual matched mapping");
        auto stat = repo.GetStat(InputScheme::Pinyin, input, target);
        AssertCondition(stat && stat->selected_count == 1, "Prefix learning uses original input");
    }
    auto exactA = repo.AddEntry(L"啊", {{InputScheme::Pinyin, "a"}});
    auto q = co.Query("a");
    AssertCondition(!q.candidates.empty() && q.candidates.front().entry_id == exactA &&
        q.candidates.front().quality == MatchQuality::ExactFull, "Exact stage still precedes completion");

    const std::string path = "out/stage4a-finalization-dict";
    std::filesystem::create_directories(path);
    {
        std::ofstream file(path + "/pinyin.dict");
        file << "One\txian\txian\t10\nTwo\txian\txi an\t10\n";
    }
    AssertCondition(dict.LoadPinyin(path + "/pinyin.dict"), "Boundary fixture loaded");
    policy.dictionary_order = {{DICT_ID_BUILTIN_PINYIN, true}};
    co.SetPolicy(policy);
    for (int recalled = 0; recalled < 2; ++recalled) {
        if (recalled) {
            AssertCondition(repo.EnsureBuiltinEntryExists("b_py_xian_One", L"One"), "Recall metadata created");
            AssertCondition(co.SetPinPreference(InputScheme::Pinyin, "xi'an", "b_py_xian_One", true, 1), "Conflicting pin inserted");
            AssertCondition(repo.SetStatDirect(InputScheme::Pinyin, "xi'an", "b_py_xian_One",
                100, clock->Now(), 100.0, clock->Now()), "Conflicting active stats inserted");
        }
        q = co.Query("xi'an");
        AssertCondition(q.status == CoordinatorStatus::Success && q.candidates.size() == 1 &&
            q.candidates[0].text == L"Two", "Explicit full-pinyin boundary excludes incompatible normal and recalled candidates");
    }
    std::cout << "  [PASS] Finalization prefixes, learning and explicit recall boundaries verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 20: Input Method Independent Enabling / Disabling & Validation
static void TestInputMethodEnablingDisablingAndValidation(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 20] Testing Input Method Independent Enabling/Disabling & Validation..." << std::endl;

    // 1. Initial policy: Wubi86 (primary), Pinyin, Shuangpin in order, all enabled
    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.mixed_input_enabled = true;
    policy.input_method_order = {InputScheme::Wubi86, InputScheme::Pinyin, InputScheme::Shuangpin};
    policy.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_WUBI, true},
        {DICT_ID_BUILTIN_PINYIN, true}
    };
    AssertCondition(policy.IsMethodEnabled(InputScheme::Wubi86), "Wubi86 enabled by default");
    AssertCondition(policy.IsMethodEnabled(InputScheme::Pinyin), "Pinyin enabled by default");
    AssertCondition(policy.IsMethodEnabled(InputScheme::Shuangpin), "Shuangpin enabled by default");

    // Add a custom Shuangpin entry for testing: code "xl" (Xiaohe Shuangpin for "xiang")
    auto spEntryId = repo.AddEntry(L"想", {{InputScheme::Pinyin, "xiang"}});
    AssertCondition(!spEntryId.empty(), "Added test entry for pinyin xiang");

    // With Shuangpin enabled, querying "xl" in Xiaohe layout yields "想"
    policy.shuangpin_layout = ShuangpinLayout::Xiaohe;
    coordinator.SetPolicy(policy);
    auto qEnabled = coordinator.Query("xl");
    bool hasSpMatch = false;
    for (const auto& c : qEnabled.candidates) {
        if (c.entry_id == spEntryId && c.scheme == InputScheme::Shuangpin) {
            hasSpMatch = true;
            break;
        }
    }
    AssertCondition(hasSpMatch, "Query 'xl' returns Shuangpin candidate when Shuangpin is enabled");

    // 2. Disable Shuangpin: ensure it NEVER participates in mixed input, cascading fallback, or resolution!
    policy.SetMethodEnabled(InputScheme::Shuangpin, false);
    AssertCondition(!policy.IsMethodEnabled(InputScheme::Shuangpin), "Shuangpin is now disabled");
    coordinator.SetPolicy(policy);

    auto qDisabled = coordinator.Query("xl");
    for (const auto& c : qDisabled.candidates) {
        AssertCondition(c.scheme != InputScheme::Shuangpin, "Disabled Shuangpin must NEVER return candidates");
    }

    // 3. Validation: Primary method MUST be enabled
    InputPolicy invalidPrimaryPolicy = policy;
    invalidPrimaryPolicy.primary_method = InputScheme::Wubi86;
    invalidPrimaryPolicy.SetMethodEnabled(InputScheme::Wubi86, false);
    std::string err;
    AssertCondition(!ValidateInputPolicy(invalidPrimaryPolicy, &err), "Disabling primary method must fail validation");
    AssertCondition(err.find("Default (primary) input method must be enabled") != std::string::npos, "Error specifies primary method must be enabled");

    // 4. Validation: At least one input method must be enabled
    InputPolicy allDisabledPolicy = policy;
    allDisabledPolicy.SetMethodEnabled(InputScheme::Wubi86, false);
    allDisabledPolicy.SetMethodEnabled(InputScheme::Pinyin, false);
    allDisabledPolicy.SetMethodEnabled(InputScheme::Shuangpin, false);
    AssertCondition(!ValidateInputPolicy(allDisabledPolicy, &err), "Disabling all input methods must fail validation");

    // 5. JSON serialization & deserialization roundtrip
    auto j = InputPolicyToJson(policy);
    AssertCondition(j.contains("enabled_input_methods"), "JSON contains enabled_input_methods");
    AssertCondition(j.contains("input_methods"), "JSON contains input_methods");
    AssertCondition(j.contains("input_method_order"), "JSON preserves input_method_order");

    InputPolicy parsedPolicy;
    AssertCondition(InputPolicyFromJson(j, parsedPolicy, &err), "Parse policy with disabled Shuangpin succeeded");
    AssertCondition(parsedPolicy.IsMethodEnabled(InputScheme::Wubi86), "Parsed Wubi86 enabled");
    AssertCondition(parsedPolicy.IsMethodEnabled(InputScheme::Pinyin), "Parsed Pinyin enabled");
    AssertCondition(!parsedPolicy.IsMethodEnabled(InputScheme::Shuangpin), "Parsed Shuangpin disabled");

    // 6. Only old field (enabled_input_methods)
    {
        nlohmann::json oldOnly = j;
        oldOnly.erase("input_methods");
        oldOnly["enabled_input_methods"] = nlohmann::json::array({"wubi86"});
        InputPolicy pOld;
        AssertCondition(InputPolicyFromJson(oldOnly, pOld, &err), "Parse only enabled_input_methods");
        AssertCondition(pOld.IsMethodEnabled(InputScheme::Wubi86), "Wubi86 enabled in oldOnly");
        AssertCondition(!pOld.IsMethodEnabled(InputScheme::Pinyin), "Pinyin disabled in oldOnly");
        AssertCondition(!pOld.IsMethodEnabled(InputScheme::Shuangpin), "Shuangpin disabled in oldOnly");
    }

    // 7. Only new field (input_methods)
    {
        nlohmann::json newOnly = j;
        newOnly.erase("enabled_input_methods");
        newOnly["input_methods"] = nlohmann::json::array({
            {{"scheme", "wubi86"}, {"enabled", true}},
            {{"scheme", "pinyin"}, {"enabled", true}},
            {{"scheme", "shuangpin"}, {"enabled", false}}
        });
        InputPolicy pNew;
        AssertCondition(InputPolicyFromJson(newOnly, pNew, &err), "Parse only input_methods");
        AssertCondition(pNew.IsMethodEnabled(InputScheme::Wubi86), "Wubi86 enabled in newOnly");
        AssertCondition(pNew.IsMethodEnabled(InputScheme::Pinyin), "Pinyin enabled in newOnly");
        AssertCondition(!pNew.IsMethodEnabled(InputScheme::Shuangpin), "Shuangpin disabled in newOnly");
    }

    // 8. Both fields matching
    {
        nlohmann::json matching = j;
        matching["enabled_input_methods"] = nlohmann::json::array({"wubi86", "shuangpin"});
        matching["input_methods"] = nlohmann::json::array({
            {{"scheme", "wubi86"}, {"enabled", true}},
            {{"scheme", "pinyin"}, {"enabled", false}},
            {{"scheme", "shuangpin"}, {"enabled", true}}
        });
        InputPolicy pMatching;
        AssertCondition(InputPolicyFromJson(matching, pMatching, &err), "Parse matching both fields");
        AssertCondition(pMatching.IsMethodEnabled(InputScheme::Wubi86), "Wubi86 enabled in matching");
        AssertCondition(!pMatching.IsMethodEnabled(InputScheme::Pinyin), "Pinyin disabled in matching");
        AssertCondition(pMatching.IsMethodEnabled(InputScheme::Shuangpin), "Shuangpin enabled in matching");
    }

    // 9. Both fields conflicting (must fail with explicit error)
    {
        nlohmann::json conflicting = j;
        conflicting["enabled_input_methods"] = nlohmann::json::array({"wubi86"});
        conflicting["input_methods"] = nlohmann::json::array({
            {{"scheme", "wubi86"}, {"enabled", true}},
            {{"scheme", "pinyin"}, {"enabled", true}},
            {{"scheme", "shuangpin"}, {"enabled", false}}
        });
        InputPolicy pConflicting;
        AssertCondition(!InputPolicyFromJson(conflicting, pConflicting, &err), "Conflicting fields must be rejected");
        AssertCondition(err.find("Conflicting enabled states") != std::string::npos, "Error describes conflicting fields");
    }

    // Re-enable Shuangpin and cleanup
    policy.SetMethodEnabled(InputScheme::Shuangpin, true);
    coordinator.SetPolicy(policy);
    repo.DeleteEntry(spEntryId);

    std::cout << "  [PASS] Input method enabling/disabling, validation, and JSON roundtrip verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 21: Stage 6 Multi-Source Waterfall Recall, Fallback Threshold & Secondary Reverse Lookup
// -----------------------------------------------------------------------------
static void TestFallbackThresholdAndSecondaryReverseLookup(InputCoordinator& coordinator, EntryRepository& repo) {
    std::cout << "[TEST 21] Testing Stage 6 Waterfall Recall, Fallback Threshold & Reverse Lookup..." << std::endl;

    InputPolicy policy;
    policy.primary_method = InputScheme::Wubi86;
    policy.mixed_input_enabled = true;
    policy.fallback_candidate_threshold = 3;
    policy.input_method_order = {InputScheme::Wubi86, InputScheme::Pinyin, InputScheme::Shuangpin};
    policy.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_WUBI, true},
        {DICT_ID_BUILTIN_PINYIN, true}
    };
    coordinator.SetPolicy(policy);

    // 1. Custom overlay does NOT block builtin Wubi:
    // Add 1 custom entry for 'aaaa'. Both the custom entry AND builtin Wubi '工' must appear!
    std::string custId = repo.AddEntry(L"我的专属工", {{InputScheme::Wubi86, "aaaa"}});
    auto qOverlay = coordinator.Query("aaaa");
    AssertCondition(qOverlay.status == CoordinatorStatus::Success, "Query 'aaaa' succeeded");
    bool hasCustom = false;
    bool hasBuiltinGong = false;
    for (const auto& c : qOverlay.candidates) {
        if (c.text == L"我的专属工") hasCustom = true;
        if (c.text == L"工") hasBuiltinGong = true;
    }
    AssertCondition(hasCustom && hasBuiltinGong, "Custom dictionary acts as overlay and does not block builtin Wubi");
    AssertCondition(qOverlay.candidates[0].text == L"我的专属工", "Custom exact match ranks ahead of builtin exact match");
    repo.DeleteEntry(custId);

    // 2. Primary Wubi has < 3 exact matches for 'ni' ('展' is the only Wubi exact match for 'ni').
    // Because 1 < fallback_candidate_threshold (3), Pinyin exact matches ('你', '泥', etc.) MUST be supplemented,
    // ranked right after Wubi exact match '展' (ahead of Wubi prefix completions), and annotated with Wubi reverse lookup codes!
    auto qNi = coordinator.Query("ni");
    AssertCondition(qNi.status == CoordinatorStatus::Success, "Query 'ni' succeeded");
    AssertCondition(qNi.candidates.size() >= 2, "Query 'ni' has primary and supplemented secondary candidates");
    AssertCondition(qNi.candidates[0].scheme == InputScheme::Wubi86 && qNi.candidates[0].quality == MatchQuality::ExactFull,
                    "Candidate #0 is Wubi86 ExactFull ('展')");
    AssertCondition(qNi.candidates[0].reverse_lookup_code.empty(),
                    "Primary input method candidate does NOT have redundant reverse lookup annotation");

    bool foundPinyinNi = false;
    for (const auto& c : qNi.candidates) {
        if (c.text == L"你" && c.scheme == InputScheme::Pinyin) {
            foundPinyinNi = true;
            AssertCondition(!c.reverse_lookup_code.empty(), "Supplemented Pinyin candidate '你' has Wubi86 reverse_lookup_code populated");
            break;
        }
    }
    AssertCondition(foundPinyinNi, "Pinyin candidate '你' supplemented when Wubi exact count (1) < threshold (3)");

    // 3. When fallback_candidate_threshold is set to 1, 'ni' (which has 1 Wubi exact match >= 1) does NOT cascade to builtin Pinyin!
    policy.fallback_candidate_threshold = 1;
    coordinator.SetPolicy(policy);
    auto qNiThresh1 = coordinator.Query("ni");
    bool foundPinyinWhenThresh1 = false;
    for (const auto& c : qNiThresh1.candidates) {
        if (c.dict_id == DICT_ID_BUILTIN_PINYIN) {
            foundPinyinWhenThresh1 = true;
            break;
        }
    }
    AssertCondition(!foundPinyinWhenThresh1, "When threshold=1 and Wubi has 1 exact match, builtin Pinyin is not supplemented");

    // Restore threshold=3
    policy.fallback_candidate_threshold = 3;
    coordinator.SetPolicy(policy);

    std::cout << "  [PASS] Stage 6 Waterfall Recall, Fallback Threshold & Secondary Reverse Lookup verified!" << std::endl;
}

// Main Test Runner
// -----------------------------------------------------------------------------
int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  PersonalIME Stage 4A Input Policy & Shuangpin Test    " << std::endl;
    std::cout << "========================================================" << std::endl;

    // Run independent shuangpin tests first (no database needed)
    TestShuangpinStandardVectors();
    TestReviewRound2P1_4BoundedCombinatorialDecoding();

    // Prepare isolated temporary database
    std::string testDb = "stage4a_test.db";
    if (std::filesystem::exists(testDb)) {
        std::filesystem::remove(testDb);
    }

    SqliteDb db;
    bool dbOk = db.Open(testDb);
    AssertCondition(dbOk, "Open isolated test database");
    bool migOk = Migration::Migrate(db);
    AssertCondition(migOk, "Migrate isolated test database to latest schema");

    auto clock = std::make_shared<SystemClock>();
    EntryRepository repo(db, clock);

    BuiltinDictionary builtinDict;
    bool dictLoaded = builtinDict.Load("data/dictionaries");
    AssertCondition(dictLoaded, "Load builtin wubi and pinyin dictionaries");

    InputCoordinator coordinator(repo, builtinDict, clock);

    // Run all comprehensive test suites
    TestInputMethodAndMixedIsolation(coordinator);
    TestCascadingFallbackAndExactOverPrefix(coordinator, repo);
    TestManualDictionaryNavigationWithEmptyMiddle(coordinator, repo);
    TestLearningDisabledAndCommitMatrix(coordinator, repo, clock);
    TestShuangpinAndBuiltinCommits(coordinator, repo, clock);
    TestPinAndStatsScopeIsolation(coordinator, repo);
    TestShuangpinIncompleteAndIllegalValidation(coordinator);
    TestStorageErrorDistinction(builtinDict, clock);
    TestInputMethodPriorityInRanking(coordinator, repo);
    TestConcurrentQueryThreadSafety(coordinator);
    TestReviewRound2P1_1RecallExactFilter(repo, clock);
    TestReviewRound2P1_2ShuangpinScopedRecall(repo, clock);
    TestReviewRound2P1_3ShuangpinSyllableBoundaries(repo, clock);
    TestReviewRound3CustomSyllablesAndShortText(repo, clock);
    TestReviewRound3TruncationPropagationAndBoundaries(repo, clock);
    TestReviewRound4Regressions(repo, clock);
    TestFinalizationPinyinPrefixesAndBoundaries();
    TestInputMethodEnablingDisablingAndValidation(coordinator, repo);
    TestFallbackThresholdAndSecondaryReverseLookup(coordinator, repo);

    db.Close();
    std::filesystem::remove(testDb);

    std::cout << "========================================================" << std::endl;
    std::cout << "  ALL Stage 4A Input Policy & Shuangpin Tests PASSED!   " << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
