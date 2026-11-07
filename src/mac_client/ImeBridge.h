#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// One candidate produced by the ime-core engine, already converted to NSString
/// so Swift and Objective-C call sites never touch std::wstring.
@interface ImeCandidate : NSObject

@property (nonatomic, copy, readonly) NSString *entryId;
@property (nonatomic, copy, readonly) NSString *text;
@property (nonatomic, assign, readonly) NSInteger index;

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                          index:(NSInteger)index NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

/// Thin Objective-C++ wrapper around ime::ImeEngine.
/// All methods are safe to call from the main thread; queries and commits are
/// serialized on an internal queue because the engine is not re-entrant.
@interface ImeEngineBridge : NSObject

@property (nonatomic, assign, readonly, getter=isReady) BOOL ready;
@property (nonatomic, copy, readonly) NSString *databasePath;
@property (nonatomic, copy, readonly) NSString *dictionaryPath;

+ (instancetype)sharedBridge;

/// Opens the database and loads dictionaries on first successful call.
- (BOOL)initializeIfNeeded;

/// Queries candidates. `scheme` accepts "pinyin", "wubi", "custom", "shuangpin".
- (NSArray<ImeCandidate *> *)queryWithScheme:(NSString *)scheme code:(NSString *)code;

/// Records a successful commit so the ranker can learn from it.
- (BOOL)commitWithEventId:(NSString *)eventId
                    scheme:(NSString *)scheme
                      code:(NSString *)code
                   entryId:(NSString *)entryId;

@end

NS_ASSUME_NONNULL_END
