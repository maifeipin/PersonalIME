#import <Foundation/Foundation.h>
#import "../src/mac_client/ImeBridge.h"
#include <iostream>
#include <cstdlib>

#define TEST_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "Assertion failed: " #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        std::cout << "=== Running macOS ImeBridge Feature & Policy Verification ===" << std::endl;

        NSString *tempDir = NSTemporaryDirectory();
        NSString *testDbPath = [tempDir stringByAppendingPathComponent:[NSString stringWithFormat:@"test_mac_%u_%lld.db", arc4random(), (long long)[[NSDate date] timeIntervalSince1970]]];

        NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
        NSString *dictDir = [cwd stringByAppendingPathComponent:@"data/dictionaries"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:[dictDir stringByAppendingPathComponent:@"pinyin.dict"]]) {
            dictDir = [[cwd stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"data/dictionaries"];
        }

        setenv("PERSONALIME_DB_PATH", testDbPath.UTF8String, 1);
        setenv("PERSONALIME_DICT_DIR", dictDir.UTF8String, 1);

        ImeEngineBridge *bridge = [ImeEngineBridge sharedBridge];
        BOOL initialized = [bridge initializeIfNeeded];
        TEST_ASSERT(initialized);
        TEST_ASSERT(bridge.isReady);
        std::cout << "[PASS] ImeEngineBridge initialized successfully" << std::endl;

        // 1. Policy defaults
        TEST_ASSERT([[bridge primaryScheme] isEqualToString:@"wubi"]);
        TEST_ASSERT([bridge mixedInputEnabled] == YES);
        std::cout << "[PASS] Default policy verified (wubi + mixed input)" << std::endl;

        // 2. Wubi Query
        NSArray<ImeCandidate *> *wubiCands = [bridge queryCoordinatorWithCode:@"aaaa"];
        TEST_ASSERT(wubiCands.count > 0);
        TEST_ASSERT([wubiCands[0].text isEqualToString:@"工"]);
        std::cout << "[PASS] Wubi query verified ('aaaa' -> '工')" << std::endl;

        // 3. Reverse lookup and mixed input (typing pinyin under Wubi primary mode)
        NSArray<ImeCandidate *> *pinyinUnderWubi = [bridge queryCoordinatorWithCode:@"zhongwen"];
        TEST_ASSERT(pinyinUnderWubi.count > 0);
        std::cout << "[PASS] Waterfall recall verified ('zhongwen' returned " << pinyinUnderWubi.count << " candidates)" << std::endl;

        // 4. Smart macros: "date", "sj" (time)
        NSArray<ImeCandidate *> *macroCands = [bridge queryCoordinatorWithCode:@"date"];
        TEST_ASSERT(macroCands.count > 0);
        std::cout << "[PASS] Smart dynamic macro verified ('date' -> '" << [macroCands[0].text UTF8String] << "')" << std::endl;

        // 5. Commit candidate
        TEST_ASSERT([bridge commitCandidateAtIndex:0]);
        std::cout << "[PASS] commitCandidateAtIndex verified" << std::endl;

        // 6. Policy switching
        [bridge setPrimaryScheme:@"pinyin"];
        TEST_ASSERT([[bridge primaryScheme] isEqualToString:@"pinyin"]);
        NSArray<ImeCandidate *> *pyCands = [bridge queryCoordinatorWithCode:@"nihao"];
        TEST_ASSERT(pyCands.count > 0);
        TEST_ASSERT([pyCands[0].text isEqualToString:@"你好"]);
        std::cout << "[PASS] Primary scheme switch to pinyin verified ('nihao' -> '你好')" << std::endl;

        // 7. Shuangpin layout switching
        [bridge setPrimaryScheme:@"shuangpin"];
        [bridge setShuangpinLayout:@"natural"];
        TEST_ASSERT([[bridge shuangpinLayout] isEqualToString:@"natural"]);
        NSArray<ImeCandidate *> *spCands = [bridge queryCoordinatorWithCode:@"vi"];
        TEST_ASSERT(spCands.count > 0);
        std::cout << "[PASS] Shuangpin query verified ('vi' -> '" << [spCands[0].text UTF8String] << "')" << std::endl;

        // 8. Wordbook management
        NSString *eid = [bridge addUserEntryWithText:@"人人输入法" scheme:@"wubi" code:@"ffff"];
        TEST_ASSERT(eid.length > 0);
        NSArray<ImeUserEntry *> *entries = [bridge allUserEntries];
        BOOL found = NO;
        for (ImeUserEntry *e in entries) {
            if ([e.text isEqualToString:@"人人输入法"] && [e.code isEqualToString:@"ffff"]) {
                found = YES;
                break;
            }
        }
        TEST_ASSERT(found);
        std::cout << "[PASS] User custom wordbook addition verified ('人人输入法')" << std::endl;

        TEST_ASSERT([bridge deleteUserEntryWithId:eid]);
        std::cout << "[PASS] User custom wordbook deletion verified" << std::endl;

        // 9. Statistics
        ImeStatistics *stats = [bridge statistics];
        TEST_ASSERT(stats.wubiCount > 100000);
        TEST_ASSERT(stats.pinyinCount > 50000);
        std::cout << "[PASS] Statistics verified (Wubi: " << stats.wubiCount << ", Pinyin: " << stats.pinyinCount << ")" << std::endl;

        // 10. Backup export / import
        NSString *exportPath = [tempDir stringByAppendingPathComponent:@"mac_test_export.json"];
        NSError *expErr = nil;
        TEST_ASSERT([bridge exportDataToPath:exportPath error:&expErr]);
        std::cout << "[PASS] Data export verified (JSON created at " << [exportPath UTF8String] << ")" << std::endl;

        NSError *impErr = nil;
        TEST_ASSERT([bridge importDataFromPath:exportPath replaceMode:NO error:&impErr]);
        std::cout << "[PASS] Data import verified" << std::endl;

        [[NSFileManager defaultManager] removeItemAtPath:testDbPath error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:exportPath error:nil];

        std::cout << "=== ALL macOS ImeBridge Feature & Policy Verifications PASSED! ===" << std::endl;
    }
    return 0;
}
