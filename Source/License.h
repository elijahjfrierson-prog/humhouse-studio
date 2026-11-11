#pragma once
#include <juce_core/juce_core.h>

// HumHouse Studio licensing: RSA-2048 signed keys (PKCS#1 v1.5 / SHA-256), locked to up to 2 Machine IDs.
namespace hhs::license
{
struct Info
{
    bool ok = false;
    juce::String error, name, email, licId;
};
juce::String machineId();
Info verifyFor (const juce::String& key, const juce::String& machine);
inline Info verify (const juce::String& key) { return verifyFor (key, machineId()); }
juce::File keyFile();
bool storedValid();

// Online activation: the license server (our own) hands out a signed pass locked to this Machine ID.
// A key holds up to 3 computers; the pass lasts a day and renews silently in the background (revoke locks it).
struct Pass
{
    bool ok = false, expired = false;   // expired: genuine pass that just needs its daily online check
    juce::String error;
    juce::int64 issued = 0, expires = 0;
};
Pass verifyPass (const juce::String& pass, const juce::String& machine, juce::int64 nowSecs);
juce::File passFile();
Pass storedPass();
juce::String serverUrl();
juce::String homeServerUrl();   // the owner's own License Server, if it's online (Cloudflare knows its address)

struct Online
{
    bool ok = false, lockNow = false;   // lockNow: revoked / unknown key / no free computer slot
    bool provisional = false;           // server down/busy but the key is one of ours: unlocked, confirmed online later
    juce::String error, pass;
};
Online activateOnline (const juce::String& key, const juce::String& machine);   // blocking: call off the message thread
bool saveActivation (const juce::String& key, const juce::String& pass);   // empty pass: keeps the key only (provisional)
juce::String findKey (const juce::String& text);   // a key anywhere in pasted text -> exactly HHS-XXXXX-XXXXX-XXXXX-XXXXX, or empty
bool knownKey (const juce::String& key);           // one of our sold keys (built-in fingerprints, no internet needed)
bool graceOk();                                    // saved known key unlocked while the server was down, within 7 days
void clearActivation();
}
