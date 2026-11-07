#import "ImeBridge.h"

#include "core/engine/ime_engine.h"

#include <memory>
#include <string>
#include <vector>

@implementation ImeCandidate

- (instancetype)initWithEntryId:(NSString *)entryId
                           text:(NSString *)text
                          index:(NSInteger)index {
    self = [super init];
    if (self) {
        _entryId = [entryId copy];
        _text = [text copy];
        _index = index;
    }
    return self;
}

@end

@implementation ImeEngineBridge {
    std::unique_ptr<ime::ImeEngine> _engine;
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
    return [[dir URLByAppendingPathComponent:@"personal_ime.db"] path];
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
            [result addObject:[[ImeCandidate alloc] initWithEntryId:entryId
                                                               text:text
                                                              index:(NSInteger)i]];
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

@end
