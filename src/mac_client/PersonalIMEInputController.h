#pragma once

#import <InputMethodKit/InputMethodKit.h>

/// Input method controller backing the macOS shell. It owns the composition
/// buffer, talks to ime-core through ImeEngineBridge and drives the SwiftUI
/// candidate panel through CandidatePanelController.
@interface PersonalIMEInputController : IMKInputController
@end
