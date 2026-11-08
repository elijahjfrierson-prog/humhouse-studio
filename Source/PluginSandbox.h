#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace hhs
{
#if ! JUCE_IOS
// Third-party plug-ins run in a helper copy of the app: if one crashes, only that plug-in goes quiet, the song keeps playing.
std::unique_ptr<juce::AudioPluginInstance> createSandboxedPlugin (const juce::PluginDescription&, double sampleRate, int maxBlock, juce::String& error);
bool isSandboxed (const juce::AudioPluginInstance&);
bool sandboxCrashed (const juce::AudioPluginInstance&);
bool isPluginHostChild (const juce::String& commandLine);
bool runPluginHostChild (const juce::String& commandLine);   // true: keep the helper app running
#endif
}
