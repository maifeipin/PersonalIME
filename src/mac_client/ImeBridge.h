#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// One candidate produced by the engine for display and selection.
@interface ImeCandidate : NSObject

@property (nonatomic, copy, readonly) NSString *entryId;
@property (nonatomic, copy, readonly) NSString *text;
@property (nonatomic, copy, readonly) NSString *matchedCode;
@property (nonatomic, copy, readonly) NSString *reverseLookupCode; // e.g. "wq", rendered as [wq]
@property (nonatomic, copy, readonly) NSString *dictId;
@property (nonatomic, assign, readonly) NSInteger index;
@property (nonatomic, assign, readonly) BOOL isPinned;

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                    matchedCode:(NSString *)matchedCode
              reverseLookupCode:(NSString *)reverseLookupCode
                         dictId:(NSString *)dictId
                          index:(NSInteger)index
                       isPinned:(BOOL)isPinned NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

/// User custom dictionary entry for wordbook management.
@interface ImeUserEntry : NSObject
@property (nonatomic, copy, readonly) NSString *entryId;
@property (nonatomic, copy, readonly) NSString *text;
@property (nonatomic, copy, readonly) NSString *scheme;
@property (nonatomic, copy, readonly) NSString *code;

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                         scheme:(NSString *)scheme
                           code:(NSString *)code NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@end

/// Dictionary and usage statistics.
@interface ImeStatistics : NSObject
@property (nonatomic, assign, readonly) NSInteger wubiCount;
@property (nonatomic, assign, readonly) NSInteger pinyinCount;
@property (nonatomic, assign, readonly) NSInteger userEntryCount;

- (instancetype)initWithWubiCount:(NSInteger)wubi
                      pinyinCount:(NSInteger)pinyin
                   userEntryCount:(NSInteger)user NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@end

/// Objective-C++ bridge wrapping ime::ImeEngine and ime::InputCoordinator.
@interface ImeEngineBridge : NSObject

@property (nonatomic, assign, readonly, getter=isReady) BOOL ready;
@property (nonatomic, copy, readonly) NSString *databasePath;
@property (nonatomic, copy, readonly) NSString *dictionaryPath;

// --- Policy Settings ---
@property (nonatomic, copy) NSString *primaryScheme;        // "wubi", "pinyin", "shuangpin"
@property (nonatomic, copy) NSString *shuangpinLayout;      // "natural", "ms", "xiaohe"
@property (nonatomic, assign) BOOL mixedInputEnabled;
@property (nonatomic, assign) NSInteger pageSize;           // 5..9
@property (nonatomic, assign) NSInteger fallbackThreshold;  // 0..9
@property (nonatomic, assign) BOOL frequencyEnabled;
@property (nonatomic, assign) BOOL learningEnabled;

+ (instancetype)sharedBridge;

/// Opens SQLite database and loads builtin dictionaries.
- (BOOL)initializeIfNeeded;

/// High-level query driven by InputCoordinator and active InputPolicy.
- (NSArray<ImeCandidate *> *)queryCoordinatorWithCode:(NSString *)code;

/// Commits candidate at the given index from the last query result.
- (BOOL)commitCandidateAtIndex:(NSInteger)index;

/// Explicit scheme-level query (fallback / tests).
- (NSArray<ImeCandidate *> *)queryWithScheme:(NSString *)scheme code:(NSString *)code;

/// Legacy commit interface.
- (BOOL)commitWithEventId:(NSString *)eventId
                    scheme:(NSString *)scheme
                      code:(NSString *)code
                   entryId:(NSString *)entryId;

// --- Wordbook Management ---
- (nullable NSString *)addUserEntryWithText:(NSString *)text scheme:(NSString *)scheme code:(NSString *)code;
- (BOOL)deleteUserEntryWithId:(NSString *)entryId;
- (NSArray<ImeUserEntry *> *)allUserEntries;

// --- Pinning ---
- (BOOL)pinCandidateWithScheme:(NSString *)scheme code:(NSString *)code entryId:(NSString *)entryId rank:(NSInteger)rank;
- (BOOL)unpinCandidateWithScheme:(NSString *)scheme code:(NSString *)code entryId:(NSString *)entryId;

// --- Backup & Maintenance ---
- (BOOL)exportDataToPath:(NSString *)filePath error:(NSError **)error;
- (BOOL)importDataFromPath:(NSString *)filePath replaceMode:(BOOL)replace error:(NSError **)error;
- (BOOL)resetLearningStats;
- (ImeStatistics *)statistics;

@end

NS_ASSUME_NONNULL_END
