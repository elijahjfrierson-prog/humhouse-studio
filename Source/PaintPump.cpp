#include "UI.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Windows only sends WM_PAINT when the message queue is empty, so a flood of mouse moves (or OBS / Discord hooks) could
// hold back every repaint and freeze meters, scopes and plug-in UIs. Each screen refresh paints whatever is waiting,
// including hosted plug-in child windows. macOS already paints on the display link.
namespace hhs
{
void pumpPendingPaints()
{
   #if JUCE_WINDOWS
    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
        if (auto* peer = juce::ComponentPeer::getPeer (i); peer != nullptr && peer->getComponent().isShowing())
        {
            peer->performAnyPendingRepaintsNow();
            if (i < juce::ComponentPeer::getNumPeers() && juce::ComponentPeer::getPeer (i) == peer)
                if (auto hwnd = (HWND) peer->getNativeHandle())
                    RedrawWindow (hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
   #endif
}
}
