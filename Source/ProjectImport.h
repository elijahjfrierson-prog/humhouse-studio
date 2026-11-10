#pragma once
#include "Model.h"
#include <functional>

namespace hhs
{
// returns the plugin identifier string of an installed plugin with that name, or {} if it isn't installed
using PluginFinder = std::function<juce::String (const juce::String& name, bool instrument)>;

// Reaper .rpp, Ableton .als and FL Studio .flp: tracks, audio clips, MIDI, mixer levels and third-party FX by name
bool isForeignProject (const juce::File& f);
bool importForeignProject (const juce::File& f, Project& out, juce::StringArray& report, const PluginFinder& find);
// Logic / Pro Tools: a folder of exported stems, one track + mixer insert per file, all at bar 1
bool importStemFolder (const juce::File& dir, Project& out, juce::StringArray& report);
// any picked audio files as stems (non-audio picks are skipped)
bool importStemFiles (juce::Array<juce::File> files, Project& out, juce::StringArray& report);
}
