#import <Cocoa/Cocoa.h>
#import <InputMethodKit/InputMethodKit.h>

#import "PersonalIMEInputController.h"
#import "ImeBridge.h"

static IMKServer *gIMKServer = nil;

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSDictionary *info = [NSBundle mainBundle].infoDictionary;
        NSString *connectionName = info[@"InputMethodConnectionName"];
        NSString *bundleIdentifier = [NSBundle mainBundle].bundleIdentifier;

        if (connectionName.length == 0 || bundleIdentifier.length == 0) {
            NSLog(@"PersonalIME: Info.plist is missing InputMethodConnectionName or CFBundleIdentifier");
            return 1;
        }

        // Touch the controller class so the linker never strips it; IMK resolves
        // PersonalIMEInputController by name from InputMethodServerControllerClass.
        (void)[PersonalIMEInputController class];

        gIMKServer = [[IMKServer alloc] initWithName:connectionName
                                 bundleIdentifier:bundleIdentifier];
        if (gIMKServer == nil) {
            NSLog(@"PersonalIME: IMKServer failed to start (name=%@, bundle=%@)",
                  connectionName, bundleIdentifier);
            return 1;
        }

        // Kick off engine startup early so the first keystroke is not blocked by
        // SQLite migration and dictionary loading.
        if (![[ImeEngineBridge sharedBridge] initializeIfNeeded]) {
            NSLog(@"PersonalIME: engine initialization failed (db=%@, dict=%@)",
                  [ImeEngineBridge sharedBridge].databasePath,
                  [ImeEngineBridge sharedBridge].dictionaryPath);
        }

        [[NSApplication sharedApplication] run];
    }
    return 0;
}
