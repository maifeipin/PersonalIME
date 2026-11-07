#import "PersonalIMEInputController.h"

#import "ImeBridge.h"
#import "PersonalIME-Swift.h"

#include <algorithm>

static const NSUInteger kMaxCompositionLength = 32;

@interface PersonalIMEInputController () <CandidatePanelDelegate, SettingsDataSource> {
    NSString *_composition;
    NSArray<ImeCandidate *> *_candidates;
    NSInteger _selectedIndex;   // 0-based index within the CURRENT page
    NSInteger _currentPage;     // 0-based page index
    BOOL _englishMode;
    id<IMKTextInput> _currentClient;
}
@end

@implementation PersonalIMEInputController

- (instancetype)initWithServer:(IMKServer *)server
                      delegate:(id)delegate
                        client:(id)inputClient {
    self = [super initWithServer:server delegate:delegate client:inputClient];
    if (self) {
        _composition = @"";
        _candidates = @[];
        _selectedIndex = 0;
        _currentPage = 0;
        _englishMode = NO;
        _currentClient = (id<IMKTextInput>)inputClient;
        [CandidatePanelController shared].delegate = self;
        [SettingsWindowController shared].dataSource = self;
    }
    return self;
}

#pragma mark - IMKInputController Lifecycle

- (void)activateServer:(id)sender {
    [super activateServer:sender];
    _currentClient = (id<IMKTextInput>)sender;
    [CandidatePanelController shared].delegate = self;
    [SettingsWindowController shared].dataSource = self;
    [[ImeEngineBridge sharedBridge] initializeIfNeeded];
    [self resetCompositionWithClient:sender];
}

- (void)deactivateServer:(id)sender {
    [self resetCompositionWithClient:sender];
    [[CandidatePanelController shared] hidePanel];
    [super deactivateServer:sender];
}

#pragma mark - Event Handling

- (BOOL)handleEvent:(NSEvent *)event client:(id)sender {
    _currentClient = (id<IMKTextInput>)sender;
    // Shift key toggles Chinese / English mode (matching macOS & Windows convention)
    if (event.type == NSEventTypeFlagsChanged &&
        event.keyCode == 56 /* kVK_Shift */) {
        BOOL shiftDown = (event.modifierFlags & NSEventModifierFlagShift) != 0;
        if (shiftDown) {
            [self toggleEnglishModeWithClient:sender];
            return YES;
        }
        return NO;
    }
    return [super handleEvent:event client:sender];
}

- (BOOL)inputText:(NSString *)string client:(id)sender {
    id<IMKTextInput> client = (id<IMKTextInput>)sender;
    _currentClient = client;
    if (string.length == 0) {
        return NO;
    }
    if (_englishMode) {
        return NO;
    }

    unichar ch = [string characterAtIndex:0];

    // Paging keys when composition is active: - (prev), = (next)
    if (_composition.length > 0) {
        if (ch == '-' || ch == '[') {
            [self pageBy:-1 client:client];
            return YES;
        }
        if (ch == '=' || ch == ']') {
            [self pageBy:1 client:client];
            return YES;
        }
    }

    // Letters (and ';' for Microsoft Shuangpin) extend the composition buffer
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch == ';')) {
        if (string.length != 1) {
            [self commitText:string client:client];
            return YES;
        }
        // Uppercase letters commit existing composition and bypass
        if (ch >= 'A' && ch <= 'Z') {
            if (_composition.length > 0) {
                [self commitRawCompositionWithClient:client];
            }
            return NO;
        }
        [self appendCharacter:string client:client];
        return YES;
    }

    // Space commits the highlighted candidate
    if (ch == ' ') {
        if (_composition.length > 0 && _candidates.count > 0) {
            [self commitCandidateOnPage:_selectedIndex client:client];
            return YES;
        }
        return NO;
    }

    // Digits 1-9 select candidate directly on current page
    if (ch >= '1' && ch <= '9') {
        NSInteger indexOnPage = (NSInteger)(ch - '1');
        NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
        NSInteger itemsOnPage = [self itemsOnCurrentPageWithPageSize:pageSize];
        if (_composition.length > 0 && indexOnPage < itemsOnPage) {
            [self commitCandidateOnPage:indexOnPage client:client];
            return YES;
        }
        return NO;
    }

    // Punctuation and symbols flush raw composition
    if (_composition.length > 0) {
        [self commitRawCompositionWithClient:client];
        return NO;
    }
    return NO;
}

- (BOOL)didCommandBySelector:(SEL)aSelector client:(id)sender {
    _currentClient = (id<IMKTextInput>)sender;
    if (_composition.length == 0) {
        return [super didCommandBySelector:aSelector client:sender];
    }

    if (aSelector == @selector(deleteBackward:)) {
        [self deleteBackwardWithClient:sender];
        return YES;
    }
    if (aSelector == @selector(cancelOperation:)) {
        [self resetCompositionWithClient:sender];
        [[CandidatePanelController shared] hidePanel];
        return YES;
    }
    if (aSelector == @selector(insertNewline:) ||
        aSelector == @selector(insertNewlineIgnoringFieldEditor:)) {
        [self commitRawCompositionWithClient:sender];
        return YES;
    }
    if (aSelector == @selector(pageUp:)) {
        [self pageBy:-1 client:sender];
        return YES;
    }
    if (aSelector == @selector(pageDown:)) {
        [self pageBy:1 client:sender];
        return YES;
    }
    if (aSelector == @selector(moveRight:) || aSelector == @selector(moveDown:)) {
        [self moveSelectionBy:1 client:sender];
        return YES;
    }
    if (aSelector == @selector(moveLeft:) || aSelector == @selector(moveUp:)) {
        [self moveSelectionBy:-1 client:sender];
        return YES;
    }
    return [super didCommandBySelector:aSelector client:sender];
}

#pragma mark - Composition & Paging

- (void)appendCharacter:(NSString *)character client:(id<IMKTextInput>)sender {
    if (_composition.length >= kMaxCompositionLength) {
        return;
    }
    _composition = [_composition stringByAppendingString:character.lowercaseString];
    _currentPage = 0;
    _selectedIndex = 0;
    [self refreshCandidatesWithClient:sender];
}

- (void)deleteBackwardWithClient:(id<IMKTextInput>)sender {
    if (_composition.length == 0) {
        return;
    }
    _composition = [_composition substringToIndex:_composition.length - 1];
    _currentPage = 0;
    _selectedIndex = 0;
    [self refreshCandidatesWithClient:sender];
}

- (NSInteger)itemsOnCurrentPageWithPageSize:(NSInteger)pageSize {
    NSInteger total = (NSInteger)_candidates.count;
    NSInteger start = _currentPage * pageSize;
    if (start >= total) return 0;
    return std::min(pageSize, total - start);
}

- (void)refreshCandidatesWithClient:(id<IMKTextInput>)sender {
    if (_composition.length == 0) {
        [self resetCompositionWithClient:sender];
        return;
    }

    [sender setMarkedText:_composition
           selectionRange:NSMakeRange(_composition.length, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];

    // Query coordinator with active policy
    _candidates = [[ImeEngineBridge sharedBridge] queryCoordinatorWithCode:_composition];

    if (_candidates.count == 0) {
        [[CandidatePanelController shared] hidePanel];
        return;
    }

    NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
    NSInteger totalCandidates = (NSInteger)_candidates.count;
    NSInteger totalPages = std::max((NSInteger)1, (totalCandidates + pageSize - 1) / pageSize);
    _currentPage = std::clamp(_currentPage, (NSInteger)0, totalPages - 1);
    NSInteger itemsOnPage = [self itemsOnCurrentPageWithPageSize:pageSize];
    _selectedIndex = std::clamp(_selectedIndex, (NSInteger)0, std::max((NSInteger)0, itemsOnPage - 1));

    NSInteger startIndex = _currentPage * pageSize;
    NSInteger endIndex = std::min(startIndex + pageSize, totalCandidates);

    NSMutableArray<NSString *> *texts = [NSMutableArray arrayWithCapacity:(NSUInteger)(endIndex - startIndex)];
    NSMutableArray<NSString *> *revs = [NSMutableArray arrayWithCapacity:(NSUInteger)(endIndex - startIndex)];
    NSMutableArray<NSNumber *> *pinned = [NSMutableArray arrayWithCapacity:(NSUInteger)(endIndex - startIndex)];

    for (NSInteger i = startIndex; i < endIndex; ++i) {
        ImeCandidate *cand = _candidates[(NSUInteger)i];
        [texts addObject:cand.text];
        [revs addObject:cand.reverseLookupCode ?: @""];
        [pinned addObject:@(cand.isPinned)];
    }

    [[CandidatePanelController shared] showCandidates:texts
                                         reverseCodes:revs
                                             isPinned:pinned
                                        selectedIndex:_selectedIndex
                                                 page:(int)(_currentPage + 1)
                                           totalPages:(int)totalPages
                                              atPoint:[self caretPointForClient:sender]];
}

- (void)pageBy:(NSInteger)delta client:(id<IMKTextInput>)sender {
    if (_candidates.count == 0) return;
    NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
    NSInteger totalPages = std::max((NSInteger)1, ((NSInteger)_candidates.count + pageSize - 1) / pageSize);
    NSInteger newPage = std::clamp(_currentPage + delta, (NSInteger)0, totalPages - 1);
    if (newPage != _currentPage) {
        _currentPage = newPage;
        _selectedIndex = 0;
        [self refreshCandidatesWithClient:sender];
    }
}

- (void)moveSelectionBy:(NSInteger)delta client:(id<IMKTextInput>)sender {
    if (_candidates.count == 0) return;
    NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
    NSInteger itemsOnPage = [self itemsOnCurrentPageWithPageSize:pageSize];
    NSInteger totalPages = std::max((NSInteger)1, ((NSInteger)_candidates.count + pageSize - 1) / pageSize);

    NSInteger nextIndex = _selectedIndex + delta;
    if (nextIndex >= itemsOnPage) {
        if (_currentPage < totalPages - 1) {
            _currentPage++;
            _selectedIndex = 0;
            [self refreshCandidatesWithClient:sender];
            return;
        }
    } else if (nextIndex < 0) {
        if (_currentPage > 0) {
            _currentPage--;
            NSInteger prevPageItems = [self itemsOnCurrentPageWithPageSize:pageSize];
            _selectedIndex = std::max((NSInteger)0, prevPageItems - 1);
            [self refreshCandidatesWithClient:sender];
            return;
        }
    } else {
        _selectedIndex = nextIndex;
        [[CandidatePanelController shared] updateSelection:(int)_selectedIndex
                                                      page:(int)(_currentPage + 1)
                                                totalPages:(int)totalPages];
    }
}

- (void)commitCandidateOnPage:(NSInteger)indexOnPage client:(id<IMKTextInput>)sender {
    NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
    NSInteger globalIndex = _currentPage * pageSize + indexOnPage;
    if (globalIndex < 0 || globalIndex >= (NSInteger)_candidates.count) {
        return;
    }
    ImeCandidate *candidate = _candidates[(NSUInteger)globalIndex];

    [self clearMarkedTextWithClient:sender];
    [sender insertText:candidate.text replacementRange:NSMakeRange(NSNotFound, 0)];

    [[ImeEngineBridge sharedBridge] commitCandidateAtIndex:globalIndex];

    _composition = @"";
    _candidates = @[];
    _selectedIndex = 0;
    _currentPage = 0;
    [[CandidatePanelController shared] hidePanel];
}

- (void)commitRawCompositionWithClient:(id<IMKTextInput>)sender {
    NSString *raw = _composition;
    [self clearMarkedTextWithClient:sender];
    if (raw.length > 0) {
        [sender insertText:raw replacementRange:NSMakeRange(NSNotFound, 0)];
    }
    _composition = @"";
    _candidates = @[];
    _selectedIndex = 0;
    _currentPage = 0;
    [[CandidatePanelController shared] hidePanel];
}

- (void)commitText:(NSString *)text client:(id<IMKTextInput>)sender {
    [self resetCompositionWithClient:sender];
    [sender insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)resetCompositionWithClient:(id<IMKTextInput>)sender {
    if (_composition.length > 0) {
        [self clearMarkedTextWithClient:sender];
    }
    _composition = @"";
    _candidates = @[];
    _selectedIndex = 0;
    _currentPage = 0;
}

- (void)clearMarkedTextWithClient:(id<IMKTextInput>)sender {
    [sender setMarkedText:@""
           selectionRange:NSMakeRange(0, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)toggleEnglishModeWithClient:(id)client {
    _englishMode = !_englishMode;
    if (_composition.length > 0) {
        [self resetCompositionWithClient:(id<IMKTextInput>)client];
    }
    [[CandidatePanelController shared] hidePanel];
}

#pragma mark - Caret Geometry

- (CGPoint)caretPointForClient:(id<IMKTextInput>)sender {
    NSRect lineRect = NSZeroRect;
    NSRange selected = [sender selectedRange];
    NSUInteger index = (selected.location == NSNotFound) ? 0 : selected.location;
    [sender attributesForCharacterIndex:index lineHeightRectangle:&lineRect];

    if (!NSEqualRects(lineRect, NSZeroRect)) {
        return NSMakePoint(lineRect.origin.x, lineRect.origin.y);
    }
    NSPoint mouse = NSEvent.mouseLocation;
    return NSMakePoint(mouse.x, mouse.y - 20);
}

#pragma mark - CandidatePanelDelegate

- (void)candidatePanelDidSelectWithIndexOnPage:(NSInteger)indexOnPage {
    if (_currentClient) {
        [self commitCandidateOnPage:indexOnPage client:_currentClient];
    }
}

- (void)candidatePanelDidHoverWithIndexOnPage:(NSInteger)indexOnPage {
    _selectedIndex = indexOnPage;
    NSInteger pageSize = [[ImeEngineBridge sharedBridge] pageSize];
    NSInteger totalPages = std::max((NSInteger)1, ((NSInteger)_candidates.count + pageSize - 1) / pageSize);
    [[CandidatePanelController shared] updateSelection:(int)_selectedIndex
                                                  page:(int)(_currentPage + 1)
                                            totalPages:(int)totalPages];
}

- (void)candidatePanelDidPageWithDelta:(NSInteger)delta {
    if (_currentClient) {
        [self pageBy:delta client:_currentClient];
    }
}

- (void)candidatePanelDidOpenSettings {
    [SettingsWindowController shared].dataSource = self;
    [[SettingsWindowController shared] show];
}

#pragma mark - SettingsDataSource

- (NSString *)getPrimaryScheme {
    return [[ImeEngineBridge sharedBridge] primaryScheme];
}

- (void)setPrimaryScheme:(NSString *)scheme {
    [[ImeEngineBridge sharedBridge] setPrimaryScheme:scheme];
    if (_composition.length > 0 && _currentClient) {
        [self refreshCandidatesWithClient:_currentClient];
    }
}

- (NSString *)getShuangpinLayout {
    return [[ImeEngineBridge sharedBridge] shuangpinLayout];
}

- (void)setShuangpinLayout:(NSString *)layout {
    [[ImeEngineBridge sharedBridge] setShuangpinLayout:layout];
    if (_composition.length > 0 && _currentClient) {
        [self refreshCandidatesWithClient:_currentClient];
    }
}

- (BOOL)getMixedInputEnabled {
    return [[ImeEngineBridge sharedBridge] mixedInputEnabled];
}

- (void)setMixedInputEnabled:(BOOL)enabled {
    [[ImeEngineBridge sharedBridge] setMixedInputEnabled:enabled];
    if (_composition.length > 0 && _currentClient) {
        [self refreshCandidatesWithClient:_currentClient];
    }
}

- (NSInteger)getPageSize {
    return [[ImeEngineBridge sharedBridge] pageSize];
}

- (void)setPageSize:(NSInteger)size {
    [[ImeEngineBridge sharedBridge] setPageSize:size];
    if (_composition.length > 0 && _currentClient) {
        [self refreshCandidatesWithClient:_currentClient];
    }
}

- (NSInteger)getFallbackThreshold {
    return [[ImeEngineBridge sharedBridge] fallbackThreshold];
}

- (void)setFallbackThreshold:(NSInteger)threshold {
    [[ImeEngineBridge sharedBridge] setFallbackThreshold:threshold];
    if (_composition.length > 0 && _currentClient) {
        [self refreshCandidatesWithClient:_currentClient];
    }
}

- (BOOL)getFrequencyEnabled {
    return [[ImeEngineBridge sharedBridge] frequencyEnabled];
}

- (void)setFrequencyEnabled:(BOOL)enabled {
    [[ImeEngineBridge sharedBridge] setFrequencyEnabled:enabled];
}

- (BOOL)getLearningEnabled {
    return [[ImeEngineBridge sharedBridge] learningEnabled];
}

- (void)setLearningEnabled:(BOOL)enabled {
    [[ImeEngineBridge sharedBridge] setLearningEnabled:enabled];
}

- (NSDictionary<NSString *, NSNumber *> *)getStatistics {
    ImeStatistics *stats = [[ImeEngineBridge sharedBridge] statistics];
    return @{
        @"wubi": @(stats.wubiCount),
        @"pinyin": @(stats.pinyinCount),
        @"user": @(stats.userEntryCount)
    };
}

- (BOOL)resetLearningStats {
    return [[ImeEngineBridge sharedBridge] resetLearningStats];
}

- (NSString *)exportDataTo:(NSString *)path {
    NSError *error = nil;
    if (![[ImeEngineBridge sharedBridge] exportDataToPath:path error:&error]) {
        return error.localizedDescription ?: @"Export failed";
    }
    return nil;
}

- (NSString *)importDataFrom:(NSString *)path replace:(BOOL)replace {
    NSError *error = nil;
    if (![[ImeEngineBridge sharedBridge] importDataFromPath:path replaceMode:replace error:&error]) {
        return error.localizedDescription ?: @"Import failed";
    }
    return nil;
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)allUserEntries {
    NSArray<ImeUserEntry *> *entries = [[ImeEngineBridge sharedBridge] allUserEntries];
    NSMutableArray *res = [NSMutableArray arrayWithCapacity:entries.count];
    for (ImeUserEntry *e in entries) {
        [res addObject:@{
            @"id": e.entryId ?: @"",
            @"text": e.text ?: @"",
            @"scheme": e.scheme ?: @"",
            @"code": e.code ?: @""
        }];
    }
    return res;
}

- (NSString *)addUserEntryWithText:(NSString *)text scheme:(NSString *)scheme code:(NSString *)code {
    return [[ImeEngineBridge sharedBridge] addUserEntryWithText:text scheme:scheme code:code];
}

- (BOOL)deleteUserEntryWithId:(NSString *)entryId {
    return [[ImeEngineBridge sharedBridge] deleteUserEntryWithId:entryId];
}

#pragma mark - macOS Text Input Menu

- (NSMenu *)menu {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"PersonalIME"];

    NSString *currentScheme = [[ImeEngineBridge sharedBridge] primaryScheme];

    // Input mode items
    NSMenuItem *wubiItem = [[NSMenuItem alloc] initWithTitle:@"86 五笔"
                                                      action:@selector(selectWubiMode:)
                                               keyEquivalent:@""];
    wubiItem.target = self;
    wubiItem.state = [currentScheme isEqualToString:@"wubi"] ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:wubiItem];

    NSMenuItem *pinyinItem = [[NSMenuItem alloc] initWithTitle:@"全拼"
                                                        action:@selector(selectPinyinMode:)
                                                 keyEquivalent:@""];
    pinyinItem.target = self;
    pinyinItem.state = [currentScheme isEqualToString:@"pinyin"] ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:pinyinItem];

    // Shuangpin submenu
    NSMenuItem *spItem = [[NSMenuItem alloc] initWithTitle:@"双拼" action:nil keyEquivalent:@""];
    NSMenu *spSubmenu = [[NSMenu alloc] initWithTitle:@"双拼方案"];
    NSString *spLayout = [[ImeEngineBridge sharedBridge] shuangpinLayout];

    NSArray *layouts = @[
        @{@"title": @"自然码", @"id": @"natural"},
        @{@"title": @"微软双拼", @"id": @"ms"},
        @{@"title": @"小鹤双拼", @"id": @"xiaohe"}
    ];
    for (NSDictionary *l in layouts) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:l[@"title"]
                                                      action:@selector(selectShuangpinLayout:)
                                               keyEquivalent:@""];
        item.target = self;
        item.representedObject = l[@"id"];
        if ([currentScheme isEqualToString:@"shuangpin"] && [spLayout isEqualToString:l[@"id"]]) {
            item.state = NSControlStateValueOn;
        }
        [spSubmenu addItem:item];
    }
    spItem.submenu = spSubmenu;
    if ([currentScheme isEqualToString:@"shuangpin"]) {
        spItem.state = NSControlStateValueOn;
    }
    [menu addItem:spItem];

    [menu addItem:[NSMenuItem separatorItem]];

    // Mixed input toggle
    BOOL mixed = [[ImeEngineBridge sharedBridge] mixedInputEnabled];
    NSMenuItem *mixedItem = [[NSMenuItem alloc] initWithTitle:@"智能瀑布流混输"
                                                       action:@selector(toggleMixedInput:)
                                                keyEquivalent:@""];
    mixedItem.target = self;
    mixedItem.state = mixed ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:mixedItem];

    [menu addItem:[NSMenuItem separatorItem]];

    // Settings item
    NSMenuItem *settingsItem = [[NSMenuItem alloc] initWithTitle:@"偏好设置..."
                                                          action:@selector(openSettingsWindow:)
                                                   keyEquivalent:@","];
    settingsItem.target = self;
    [menu addItem:settingsItem];

    return menu;
}

- (void)selectWubiMode:(id)sender {
    [self setPrimaryScheme:@"wubi"];
}

- (void)selectPinyinMode:(id)sender {
    [self setPrimaryScheme:@"pinyin"];
}

- (void)selectShuangpinLayout:(NSMenuItem *)sender {
    NSString *layoutId = sender.representedObject;
    if (layoutId.length > 0) {
        [self setShuangpinLayout:layoutId];
        [self setPrimaryScheme:@"shuangpin"];
    }
}

- (void)toggleMixedInput:(id)sender {
    BOOL current = [[ImeEngineBridge sharedBridge] mixedInputEnabled];
    [self setMixedInputEnabled:!current];
}

- (void)openSettingsWindow:(id)sender {
    [self candidatePanelDidOpenSettings];
}

@end
