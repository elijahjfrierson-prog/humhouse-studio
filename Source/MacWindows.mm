#include "UI.h"
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#include <pthread/qos.h>

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

// OBS in front / capturing: macOS would coalesce our timers and display-link wakeups and lower the UI thread's priority
// (App Nap, timer coalescing). A latency-critical user activity keeps every refresh on time; the UI thread runs at the
// user-interactive QoS so capture and encoder work can't starve it. The audio threads keep their real-time workgroup.
void keepUiResponsive()
{
    static id<NSObject> activity = [[NSProcessInfo processInfo] beginActivityWithOptions: (NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
                                                                                  reason: @"Real-time audio and live UI"];
   #if ! __has_feature (objc_arc)
    [activity retain];   // held for the app's lifetime
   #endif
    (void) activity;
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
}

bool windowHidden (juce::Component& c)
{
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle())
            if (NSWindow* w = [view window])
                return ([w occlusionState] & NSWindowOcclusionStateVisible) == 0;
    return false;
}

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
