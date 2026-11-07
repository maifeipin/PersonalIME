#import "ImeBridge.h"

#include "core/engine/ime_engine.h"
#include "core/strategy/input_coordinator.h"
#include "core/strategy/input_policy.h"
#include "core/encoding/shuangpin_decoder.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

@implementation ImeCandidate

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                    matchedCode:(NSString *)matchedCode
              reverseLookupCode:(NSString *)reverseLookupCode
                         dictId:(NSString *)dictId
                          index:(NSInteger)index
                       isPinned:(BOOL)isPinned {
    self = [super init];
    if (self) {
        _entryId = [entryId copy] ?: @"";
        _text = [text copy] ?: @"";
        _matchedCode = [matchedCode copy] ?: @"";
        _reverseLookupCode = [reverseLookupCode copy] ?: @"";
        _dictId = [dictId copy] ?: @"";
        _index = index;
        _isPinned = isPinned;
    }
    return self;
}

@end

@implementation ImeUserEntry

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                         scheme:(NSString *)scheme
                           code:(NSString *)code {
    self = [super init];
    if (self) {
        _entryId = [entryId copy] ?: @"";
        _text = [text copy] ?: @"";
        _scheme = [scheme copy] ?: @"";
        _code = [code copy] ?: @"";
    }
    return self;
}

@end

@implementation ImeStatistics

- (instancetype)initWithWubiCount:(NSInteger)wubi
                      pinyinCount:(NSInteger)pinyin
                   userEntryCount:(NSInteger)user {
    self = [super init];
    if (self) {
        _wubiCount = wubi;
        _pinyinCount = pinyin;
        _userEntryCount = user;
    }
    return self;
}

@end

@implementation ImeEngineBridge {
    std::unique_ptr<ime::ImeEngine> _engine;
    std::vector<ime::Candidate> _cachedQueryCandidates;
    std::string _cachedQueryRawCode;
    std::shared_ptr<const ime::InputPolicy> _cachedPolicySnapshot;
    dispatch_queue_t _queue;
    BOOL _ready;
    NSString *_dbPath;
    NSString *_dictPath;
}

+ (instancetype)sharedBridge {
    static ImeEngineBridge *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[ImeEngineBridge alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _engine = std::make_unique<ime::ImeEngine>();
        _queue = dispatch_queue_create("com.personalime.bridge", DISPATCH_QUEUE_SERIAL);
        _dbPath = [self.class resolveDatabasePath];
        _dictPath = [self.class resolveDictionaryPath];
    }
    return self;
}

- (BOOL)isReady {
    @synchronized(self) {
        return _ready;
    }
}

- (NSString *)databasePath {
    return _dbPath;
}

- (NSString *)dictionaryPath {
    return _dictPath;
}

+ (NSString *)resolveDatabasePath {
    NSString *override = NSProcessInfo.processInfo.environment[@"PERSONALIME_DB_PATH"];
    if (override.length > 0) {
        return override;
    }
    NSURL *appSupport = [[NSFileManager defaultManager]
        URLsForDirectory:NSApplicationSupportDirectory
               inDomains:NSUserDomainMask].firstObject;
    NSURL *dir = [appSupport URLByAppendingPathComponent:@"PersonalIME" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir
                             withIntermediateDirectories:YES
                                       attributes:nil
                                            error:nil];
    return [[dir URLByAppendingPathComponent:@"user_dict.db"] path];
}

+ (NSString *)resolveDictionaryPath {
    NSString *override = NSProcessInfo.processInfo.environment[@"PERSONALIME_DICT_DIR"];
    if (override.length > 0) {
        return override;
    }
    // Packaged inside the .app bundle as Contents/Resources/dictionaries.
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dictionaries"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:bundled]) {
        return bundled;
    }
    return nil;
}

- (BOOL)initializeIfNeeded {
    if (self.isReady) {
        return YES;
    }
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        if (self->_ready) {
            ok = YES;
            return;
        }
        std::string db = self->_dbPath.UTF8String ?: "";
        std::string dict = self->_dictPath.UTF8String ?: "";
        if (db.empty()) {
            return;
        }
        if (!self->_engine->Initialize(db, dict)) {
            return;
        }
        self->_ready = YES;
        ok = YES;
    });
    return ok;
}

#pragma mark - Policy Settings

- (NSString *)primaryScheme {
    if (!self.isReady || !self->_engine->GetCoordinator()) return @"wubi";
    __block NSString *res = @"wubi";
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) {
            res = [NSString stringWithUTF8String:ime::SchemeToString(snap->primary_method).c_str()];
        }
    });
    return res;
}

- (void)setPrimaryScheme:(NSString *)scheme {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.primary_method = ime::SchemeFromString(scheme.UTF8String ?: "wubi");
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (NSString *)shuangpinLayout {
    if (!self.isReady || !self->_engine->GetCoordinator()) return @"natural";
    __block NSString *res = @"natural";
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) {
            res = [NSString stringWithUTF8String:ime::ShuangpinLayoutToString(snap->shuangpin_layout).c_str()];
        }
    });
    return res;
}

- (void)setShuangpinLayout:(NSString *)shuangpinLayout {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.shuangpin_layout = ime::ShuangpinLayoutFromString(shuangpinLayout.UTF8String ?: "natural");
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (BOOL)mixedInputEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return YES;
    __block BOOL res = YES;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) {
            res = snap->mixed_input_enabled;
        }
    });
    return res;
}

- (void)setMixedInputEnabled:(BOOL)mixedInputEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.mixed_input_enabled = mixedInputEnabled;
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (NSInteger)pageSize {
    if (!self.isReady || !self->_engine->GetCoordinator()) return 5;
    __block NSInteger res = 5;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) {
            res = (NSInteger)snap->page_size;
        }
    });
    return res;
}

- (void)setPageSize:(NSInteger)pageSize {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    NSInteger clamped = std::clamp((int)pageSize, 5, 9);
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.page_size = (uint32_t)clamped;
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (NSInteger)fallbackThreshold {
    if (!self.isReady || !self->_engine->GetCoordinator()) return 3;
    __block NSInteger res = 3;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) {
            res = (NSInteger)snap->fallback_candidate_threshold;
        }
    });
    return res;
}

- (void)setFallbackThreshold:(NSInteger)fallbackThreshold {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    NSInteger clamped = std::clamp((int)fallbackThreshold, 0, 9);
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.fallback_candidate_threshold = (uint32_t)clamped;
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (BOOL)frequencyEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return YES;
    __block BOOL res = YES;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) res = snap->frequency_enabled;
    });
    return res;
}

- (void)setFrequencyEnabled:(BOOL)frequencyEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.frequency_enabled = frequencyEnabled;
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

- (BOOL)learningEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return YES;
    __block BOOL res = YES;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        if (snap) res = snap->learning_enabled;
    });
    return res;
}

- (void)setLearningEnabled:(BOOL)learningEnabled {
    if (!self.isReady || !self->_engine->GetCoordinator()) return;
    dispatch_sync(_queue, ^{
        auto snap = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::InputPolicy pol = snap ? *snap : ime::InputPolicy();
        pol.learning_enabled = learningEnabled;
        self->_engine->SavePolicy(pol);
        self->_engine->GetCoordinator()->SetPolicy(pol);
    });
}

#pragma mark - Queries and Commits

- (NSArray<ImeCandidate *> *)queryCoordinatorWithCode:(NSString *)code {
    if (!self.isReady || code.length == 0) {
        return @[];
    }
    __block NSMutableArray<ImeCandidate *> *result = nil;
    dispatch_sync(_queue, ^{
        self->_cachedQueryCandidates.clear();
        std::string raw = code.UTF8String ?: "";
        if (raw.empty() || !self->_engine->GetCoordinator()) {
            result = [NSMutableArray array];
            return;
        }
        self->_cachedQueryRawCode = raw;
        self->_cachedPolicySnapshot = self->_engine->GetCoordinator()->GetPolicySnapshot();
        ime::CoordinatorQueryResult qr = self->_engine->GetCoordinator()->Query(raw, self->_cachedPolicySnapshot);
        self->_cachedQueryCandidates = qr.candidates;
        NSUInteger count = qr.candidates.size();
        result = [NSMutableArray arrayWithCapacity:count];
        for (NSUInteger i = 0; i < count; ++i) {
            const ime::Candidate &c = qr.candidates[i];
            NSString *text = [NSString stringWithUTF8String:ime::WideToUtf8(c.text).c_str()] ?: @"";
            NSString *entryId = [NSString stringWithUTF8String:c.entry_id.c_str()] ?: @"";
            NSString *matchedCode = [NSString stringWithUTF8String:c.matched_code.c_str()] ?: @"";
            NSString *reverseLookupCode = [NSString stringWithUTF8String:c.reverse_lookup_code.c_str()] ?: @"";
            NSString *dictId = [NSString stringWithUTF8String:c.dict_id.c_str()] ?: @"";
            [result addObject:[[ImeCandidate alloc] initWithEntryId:entryId
                                                               text:text
                                                        matchedCode:matchedCode
                                                  reverseLookupCode:reverseLookupCode
                                                             dictId:dictId
                                                              index:(NSInteger)i
                                                           isPinned:c.is_pinned]];
        }
    });
    return result ?: @[];
}

- (BOOL)commitCandidateAtIndex:(NSInteger)index {
    if (!self.isReady) {
        return NO;
    }
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        if (index < 0 || index >= (NSInteger)self->_cachedQueryCandidates.size()) {
            return;
        }
        if (!self->_engine->GetCoordinator()) {
            return;
        }
        const ime::Candidate &c = self->_cachedQueryCandidates[(NSUInteger)index];
        std::string eventId = [NSUUID.UUID.UUIDString UTF8String] ?: "";
        bool learningOn = self->_cachedPolicySnapshot ? self->_cachedPolicySnapshot->learning_enabled : true;
        self->_engine->SavePendingCommit(
            eventId,
            0,
            0,
            c.scheme,
            !c.raw_code.empty() ? c.raw_code : self->_cachedQueryRawCode,
            c.entry_id,
            c.text,
            learningOn,
            c.layout_id,
            c.matched_code,
            c.resolved_code
        );
        ok = self->_engine->GetCoordinator()->CommitCandidate(c, eventId, 0, 0, self->_cachedPolicySnapshot);
    });
    return ok;
}

- (NSArray<ImeCandidate *> *)queryWithScheme:(NSString *)scheme code:(NSString *)code {
    if (!self.isReady || code.length == 0) {
        return @[];
    }
    __block NSMutableArray<ImeCandidate *> *result = nil;
    dispatch_sync(_queue, ^{
        std::string schemeStr = scheme.UTF8String ?: "";
        std::string codeStr = code.UTF8String ?: "";
        ime::InputScheme parsed = ime::SchemeFromString(schemeStr);
        if (parsed == ime::InputScheme::Unknown) {
            parsed = ime::InputScheme::Pinyin;
        }
        ime::QueryResult qr = self->_engine->Query(parsed, codeStr);
        NSUInteger count = qr.candidates.size();
        result = [NSMutableArray arrayWithCapacity:count];
        for (NSUInteger i = 0; i < count; ++i) {
            const ime::Candidate &c = qr.candidates[i];
            NSString *text = [NSString stringWithUTF8String:ime::WideToUtf8(c.text).c_str()] ?: @"";
            NSString *entryId = [NSString stringWithUTF8String:c.entry_id.c_str()] ?: @"";
            NSString *matchedCode = [NSString stringWithUTF8String:c.matched_code.c_str()] ?: @"";
            NSString *reverseLookupCode = [NSString stringWithUTF8String:c.reverse_lookup_code.c_str()] ?: @"";
            NSString *dictId = [NSString stringWithUTF8String:c.dict_id.c_str()] ?: @"";
            [result addObject:[[ImeCandidate alloc] initWithEntryId:entryId
                                                               text:text
                                                        matchedCode:matchedCode
                                                  reverseLookupCode:reverseLookupCode
                                                             dictId:dictId
                                                              index:(NSInteger)i
                                                           isPinned:c.is_pinned]];
        }
    });
    return result ?: @[];
}

- (BOOL)commitWithEventId:(NSString *)eventId
                    scheme:(NSString *)scheme
                      code:(NSString *)code
                   entryId:(NSString *)entryId {
    if (!self.isReady) {
        return NO;
    }
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        std::string schemeStr = scheme.UTF8String ?: "";
        ime::InputScheme parsed = ime::SchemeFromString(schemeStr);
        if (parsed == ime::InputScheme::Unknown) {
            parsed = ime::InputScheme::Pinyin;
        }
        ok = self->_engine->CommitCandidate(eventId.UTF8String ?: "",
                                           parsed,
                                           code.UTF8String ?: "",
                                           entryId.UTF8String ?: "");
    });
    return ok;
}

#pragma mark - Wordbook Management

- (nullable NSString *)addUserEntryWithText:(NSString *)text scheme:(NSString *)scheme code:(NSString *)code {
    if (!self.isReady) return nil;
    __block NSString *newId = nil;
    dispatch_sync(_queue, ^{
        std::wstring wText = ime::Utf8ToWide(text.UTF8String ?: "");
        std::string sCode = code.UTF8String ?: "";
        ime::InputScheme sch = ime::SchemeFromString(scheme.UTF8String ?: "");
        if (sch == ime::InputScheme::Unknown) sch = ime::InputScheme::Custom;
        std::vector<std::pair<ime::InputScheme, std::string>> codes = {{sch, sCode}};
        std::string eid = self->_engine->AddUserEntry(wText, codes);
        if (!eid.empty()) {
            newId = [NSString stringWithUTF8String:eid.c_str()];
        }
    });
    return newId;
}

- (BOOL)deleteUserEntryWithId:(NSString *)entryId {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        ok = self->_engine->DeleteUserEntry(entryId.UTF8String ?: "");
    });
    return ok;
}

- (NSArray<ImeUserEntry *> *)allUserEntries {
    if (!self.isReady) return @[];
    __block NSMutableArray<ImeUserEntry *> *entries = [NSMutableArray array];
    dispatch_sync(_queue, ^{
        auto list = self->_engine->GetAllUserEntries();
        for (const auto &e : list) {
            NSString *eid = [NSString stringWithUTF8String:e.id.c_str()] ?: @"";
            NSString *txt = [NSString stringWithUTF8String:ime::WideToUtf8(e.text).c_str()] ?: @"";
            for (const auto &c : e.codes) {
                NSString *sch = [NSString stringWithUTF8String:ime::SchemeToString(c.first).c_str()] ?: @"";
                NSString *cd = [NSString stringWithUTF8String:c.second.c_str()] ?: @"";
                [entries addObject:[[ImeUserEntry alloc] initWithEntryId:eid text:txt scheme:sch code:cd]];
            }
        }
    });
    return entries;
}

#pragma mark - Pinning

- (BOOL)pinCandidateWithScheme:(NSString *)scheme code:(NSString *)code entryId:(NSString *)entryId rank:(NSInteger)rank {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        ime::InputScheme sch = ime::SchemeFromString(scheme.UTF8String ?: "");
        ok = self->_engine->PinCandidate(sch, code.UTF8String ?: "", entryId.UTF8String ?: "", (int)rank);
    });
    return ok;
}

- (BOOL)unpinCandidateWithScheme:(NSString *)scheme code:(NSString *)code entryId:(NSString *)entryId {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        ime::InputScheme sch = ime::SchemeFromString(scheme.UTF8String ?: "");
        ok = self->_engine->UnpinCandidate(sch, code.UTF8String ?: "", entryId.UTF8String ?: "");
    });
    return ok;
}

#pragma mark - Backup & Maintenance

- (BOOL)exportDataToPath:(NSString *)filePath error:(NSError **)error {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    __block std::string errStr;
    dispatch_sync(_queue, ^{
        ok = self->_engine->ExportData(filePath.UTF8String ?: "", &errStr);
    });
    if (!ok && error) {
        *error = [NSError errorWithDomain:@"com.personalime"
                                     code:-1
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:errStr.c_str()] ?: @"Export failed"}];
    }
    return ok;
}

- (BOOL)importDataFromPath:(NSString *)filePath replaceMode:(BOOL)replace error:(NSError **)error {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    __block std::string errStr;
    dispatch_sync(_queue, ^{
        ok = self->_engine->ImportData(filePath.UTF8String ?: "", replace, &errStr);
    });
    if (!ok && error) {
        *error = [NSError errorWithDomain:@"com.personalime"
                                     code:-1
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:errStr.c_str()] ?: @"Import failed"}];
    }
    return ok;
}

- (BOOL)resetLearningStats {
    if (!self.isReady) return NO;
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        ok = self->_engine->ResetStats();
    });
    return ok;
}

- (ImeStatistics *)statistics {
    if (!self.isReady) {
        return [[ImeStatistics alloc] initWithWubiCount:0 pinyinCount:0 userEntryCount:0];
    }
    __block ImeStatistics *stats = nil;
    dispatch_sync(_queue, ^{
        NSInteger wubi = (NSInteger)self->_engine->GetBuiltinDict().WubiEntryCount();
        NSInteger pinyin = (NSInteger)self->_engine->GetBuiltinDict().PinyinEntryCount();
        NSInteger user = self->_engine->GetRepo() ? (NSInteger)self->_engine->GetRepo()->GetAllUserEntries().size() : 0;
        stats = [[ImeStatistics alloc] initWithWubiCount:wubi pinyinCount:pinyin userEntryCount:user];
    });
    return stats;
}

@end
