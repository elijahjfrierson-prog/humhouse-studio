#include "UI.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Windows only sends WM_PAINT when the message queue is empty, so a flood of mouse moves (or OBS / Discord hooks) could
// hold back every repaint and freeze meters, scopes and plug-in UIs. Each screen refresh paints whatever is waiting,
// including hosted plug-in child windows. macOS: the event-tracking loop (mouse moves, drags, OBS capture) can push the
// display link's paint back a frame or more, so each refresh flushes every window's pending paint right away.
namespace hhs
{
void pumpPendingPaints()
{
   #if JUCE_WINDOWS
    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
        if (auto* peer = juce::ComponentPeer::getPeer (i); peer != nullptr && drawsOnScreen (peer->getComponent()))
        {
            peer->performAnyPendingRepaintsNow();
            if (i < juce::ComponentPeer::getNumPeers() && juce::ComponentPeer::getPeer (i) == peer)
                if (auto hwnd = (HWND) peer->getNativeHandle())
                    RedrawWindow (hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
   #elif JUCE_MAC
    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
        if (auto* peer = juce::ComponentPeer::getPeer (i); peer != nullptr && drawsOnScreen (peer->getComponent()))
            peer->performAnyPendingRepaintsNow();
   #endif
}
}
