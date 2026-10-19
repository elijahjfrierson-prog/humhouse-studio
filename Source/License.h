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
}
