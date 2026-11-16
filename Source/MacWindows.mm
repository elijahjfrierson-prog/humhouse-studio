#include "UI.h"
#import <AppKit/AppKit.h>

// macOS full screen: pop-out Mixer / plug-in windows join the app's full-screen space (and follow it to the active space)
// instead of staying behind on the desktop, so everything works inside full screen without switching spaces.
namespace hhs
{
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
