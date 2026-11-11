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
// A key holds up to 3 computers; the pass has no time limit and is re-checked daily online (revoke locks it).
struct Pass
{
    bool ok = false;
    juce::String error;
    juce::int64 issued = 0, expires = 0;
};
Pass verifyPass (const juce::String& pass, const juce::String& machine, juce::int64 nowSecs);
juce::File passFile();
Pass storedPass();
juce::String serverUrl();

struct Online
{
    bool ok = false, lockNow = false;   // lockNow: revoked / unknown key / no free computer slot
    juce::String error, pass;
};
Online activateOnline (const juce::String& key, const juce::String& machine);   // blocking: call off the message thread
bool saveActivation (const juce::String& key, const juce::String& pass);
void clearActivation();
}
