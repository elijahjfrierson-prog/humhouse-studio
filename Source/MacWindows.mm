#include "UI.h"
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

// macOS full screen: pop-out Mixer / plug-in windows join the app's full-screen space (and follow it to the active space)
// instead of staying behind on the desktop, so everything works inside full screen without switching spaces.
namespace hhs
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void claimFileTypes()
{
    NSString* me = [[NSBundle mainBundle] bundleIdentifier];
    if (me == nil) return;
    for (NSString* ext in @[ @"hhs", @"hhpack", @"hhpreset", @"hhchain" ])
    {
        CFStringRef uti = UTTypeCreatePreferredIdentifierForTag (kUTTagClassFilenameExtension, (__bridge CFStringRef) ext, nullptr);
        if (uti == nullptr) continue;
        LSSetDefaultRoleHandlerForContentType (uti, kLSRolesAll, (__bridge CFStringRef) me);
        CFRelease (uti);
    }
}
#pragma clang diagnostic pop

void joinFullScreen (juce::Component& c)
{
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle())
            if (NSWindow* w = [view window])
            {
                const NSWindowCollectionBehavior want = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorMoveToActiveSpace;
                if (([w collectionBehavior] & want) != want)
                    [w setCollectionBehavior: ([w collectionBehavior] & ~NSWindowCollectionBehaviorFullScreenPrimary) | want];
            }
}
}
