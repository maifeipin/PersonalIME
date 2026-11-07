#import "PersonalIMEInputController.h"

#import "ImeBridge.h"
#import "PersonalIME-Swift.h"

static NSString *const kSchemePinyin = @"pinyin";
static const NSUInteger kMaxCompositionLength = 32;

@interface PersonalIMEInputController () {
    NSString *_composition;
    NSArray<ImeCandidate *> *_candidates;
    NSInteger _selectedIndex;
    BOOL _englishMode;
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
        _englishMode = NO;
    }
    return self;
}

#pragma mark - IMKInputController lifecycle

- (void)activateServer:(id)sender {
    [super activateServer:sender];
    [[ImeEngineBridge sharedBridge] initializeIfNeeded];
    [self resetCompositionWithClient:sender];
}

- (void)deactivateServer:(id)sender {
    [self resetCompositionWithClient:sender];
    [[CandidatePanelController shared] hidePanel];
    [super deactivateServer:sender];
}

#pragma mark - Event handling

- (BOOL)handleEvent:(NSEvent *)event client:(id)sender {
    // A bare Shift press toggles between Chinese and English direct mode, matching
    // the convention used by the built-in Chinese IMEs on macOS.
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
    if (string.length == 0) {
        return NO;
    }
    if (_englishMode) {
        return NO;
    }

    unichar ch = [string characterAtIndex:0];

    // Letters extend the composition buffer.
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
        if (string.length != 1) {
            // Multi-character paste: commit it verbatim.
            [self commitText:string client:client];
            return YES;
        }
        // Uppercase letters bypass the composition so Shift-assisted typing still works.
        if (ch >= 'A' && ch <= 'Z') {
            if (_composition.length > 0) {
                [self commitRawCompositionWithClient:client];
            }
            return NO;
        }
        [self appendCharacter:string client:client];
        return YES;
    }

    // Space commits the highlighted candidate.
    if (ch == ' ') {
        if (_composition.length > 0 && _candidates.count > 0) {
            [self commitCandidateAtIndex:_selectedIndex client:client];
            return YES;
        }
        return NO;
    }

    // Digits 1-9 select a candidate directly.
    if (ch >= '1' && ch <= '9') {
        NSInteger index = (NSInteger)(ch - '1');
        if (_composition.length > 0 && index < (NSInteger)_candidates.count) {
            [self commitCandidateAtIndex:index client:client];
            return YES;
        }
        return NO;
    }

    // Punctuation and everything else flushes the composition first.
    if (_composition.length > 0) {
        [self commitRawCompositionWithClient:client];
        return NO;
    }
    return NO;
}

- (BOOL)didCommandBySelector:(SEL)aSelector client:(id)sender {
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

#pragma mark - Composition

- (void)appendCharacter:(NSString *)character client:(id<IMKTextInput>)sender {
    if (_composition.length >= kMaxCompositionLength) {
        return;
    }
    _composition = [_composition stringByAppendingString:character.lowercaseString];
    [self refreshCandidatesWithClient:sender];
}

- (void)deleteBackwardWithClient:(id<IMKTextInput>)sender {
    if (_composition.length == 0) {
        return;
    }
    _composition = [_composition substringToIndex:_composition.length - 1];
    [self refreshCandidatesWithClient:sender];
}

- (void)refreshCandidatesWithClient:(id<IMKTextInput>)sender {
    if (_composition.length == 0) {
        [self resetCompositionWithClient:sender];
        return;
    }

    // Echo the raw code as marked text so the user sees what is being typed.
    [sender setMarkedText:_composition
           selectionRange:NSMakeRange(_composition.length, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];

    _candidates = [[ImeEngineBridge sharedBridge] queryWithScheme:kSchemePinyin
                                                               code:_composition];
    _selectedIndex = 0;

    if (_candidates.count == 0) {
        [[CandidatePanelController shared] hidePanel];
        return;
    }

    NSMutableArray<NSString *> *texts = [NSMutableArray arrayWithCapacity:_candidates.count];
    for (ImeCandidate *candidate in _candidates) {
        [texts addObject:candidate.text];
    }
    [[CandidatePanelController shared] showCandidates:texts
                                       selectedIndex:_selectedIndex
                                           atPoint:[self caretPointForClient:sender]];
}

- (void)moveSelectionBy:(NSInteger)delta client:(id<IMKTextInput>)sender {
    if (_candidates.count == 0) {
        return;
    }
    NSInteger count = (NSInteger)_candidates.count;
    _selectedIndex = ((_selectedIndex + delta) % count + count) % count;
    [[CandidatePanelController shared] updateSelection:_selectedIndex];
}

- (void)commitCandidateAtIndex:(NSInteger)index client:(id<IMKTextInput>)sender {
    if (index < 0 || index >= (NSInteger)_candidates.count) {
        return;
    }
    ImeCandidate *candidate = _candidates[(NSUInteger)index];
    NSString *code = _composition;

    [self clearMarkedTextWithClient:sender];
    [sender insertText:candidate.text replacementRange:NSMakeRange(NSNotFound, 0)];

    [[ImeEngineBridge sharedBridge] commitWithEventId:NSUUID.UUID.UUIDString
                                               scheme:kSchemePinyin
                                                 code:code
                                              entryId:candidate.entryId];

    _composition = @"";
    _candidates = @[];
    _selectedIndex = 0;
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
}

- (void)clearMarkedTextWithClient:(id<IMKTextInput>)sender {
    [sender setMarkedText:@""
           selectionRange:NSMakeRange(0, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)toggleEnglishModeWithClient:(id)client {
    _englishMode = !_englishMode;
    // Clearing the buffer avoids leaving a dangling composition behind.
    if (_composition.length > 0) {
        [self resetCompositionWithClient:(id<IMKTextInput>)client];
    }
    [[CandidatePanelController shared] hidePanel];
}

#pragma mark - Caret geometry

- (CGPoint)caretPointForClient:(id<IMKTextInput>)sender {
    NSRect lineRect = NSZeroRect;
    NSRange selected = [sender selectedRange];
    NSUInteger index = (selected.location == NSNotFound) ? 0 : selected.location;
    [sender attributesForCharacterIndex:index lineHeightRectangle:&lineRect];

    if (!NSEqualRects(lineRect, NSZeroRect)) {
        // IMK hands back screen coordinates with the origin at the bottom-left.
        return NSMakePoint(lineRect.origin.x, lineRect.origin.y);
    }

    // Fall back to the mouse location so the panel stays reachable.
    NSPoint mouse = NSEvent.mouseLocation;
    return NSMakePoint(mouse.x, mouse.y - 20);
}

@end
