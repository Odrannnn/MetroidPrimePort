#include "port_ios.h"

#include "port_log.h"

#include <SDL3/SDL.h>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace {
PortIos::DiscPickedCallback sCallback = nullptr;
void* sUserdata = nullptr;

void Answer(const char* path) {
    PortIos::DiscPickedCallback callback = sCallback;
    sCallback = nullptr;
    if (callback == nullptr) {
        return;
    }
    const char* const picked[] = {path, nullptr};
    const char* const none[] = {nullptr};
    callback(sUserdata, path != nullptr ? picked : none, -1);
}
} // namespace

@interface MPDiscPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

@implementation MPDiscPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* picked = urls.firstObject;
    if (picked == nil) {
        Answer(nullptr);
        return;
    }
    // asCopy puts the copy in a temporary Inbox the system may clear. Move it to
    // Documents, which is kept, backed up only if the user allows it, and shown
    // in the Files app so the copy can be deleted from there.
    const std::string documents = PortIos::DocumentsFolder();
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* target = [NSURL fileURLWithPath:[NSString stringWithUTF8String:documents.c_str()]];
    target = [target URLByAppendingPathComponent:picked.lastPathComponent];
    NSError* error = nil;
    if (![picked isEqual:target]) {
        [files removeItemAtURL:target error:nil];
        if (![files moveItemAtURL:picked toURL:target error:&error]) {
            PortLog::Write("metroid_prime_port: keeping the picked disc in place: %s\n",
                           error.localizedDescription.UTF8String);
            target = picked;
        }
    }
    Answer(target.fileSystemRepresentation);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    Answer(nullptr);
}
@end

namespace PortIos {

std::string DocumentsFolder() {
    NSURL* url = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
    if (url == nil) {
        return {};
    }
    return std::string(url.fileSystemRepresentation) + "/";
}

void ShowDiscPicker(SDL_Window* window, DiscPickedCallback callback, void* userdata) {
    // Strong for the picker's lifetime: UIDocumentPickerViewController holds
    // its delegate weakly.
    static MPDiscPickerDelegate* sDelegate = [MPDiscPickerDelegate new];
    UIWindow* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController* root = uiWindow.rootViewController;
    if (root == nil) {
        callback(userdata, nullptr, -1);
        return;
    }
    sCallback = callback;
    sUserdata = userdata;
    // Any file: disc images have no system type, and .iso's (public.iso-image)
    // would hide .rvz, .wbfs and the rest. The disc check after the pick tells
    // a wrong file apart.
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ] asCopy:YES];
    picker.delegate = sDelegate;
    picker.allowsMultipleSelection = NO;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    [root presentViewController:picker animated:YES completion:nil];
}

} // namespace PortIos
