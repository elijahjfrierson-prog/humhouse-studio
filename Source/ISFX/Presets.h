#pragma once
#include "Model.h"

namespace isfx
{
juce::File userDataDir();   // ~/Library/Application Support/InSequence FX, %APPDATA%\InSequence FX, ~/.config/InSequence FX

// ---- user effect presets (.isfxpreset): a named FxStack for the sequence look or for a clip
struct UserPreset
{
    juce::String name;
    bool clip = false;       // false = sequence look, true = per-clip effects
    FxStack fx;
    juce::File file;
};
std::vector<UserPreset> loadUserPresets (const juce::File& dir = {});
bool saveUserPreset (const UserPreset&, juce::File& written, juce::String& error, const juce::File& dir = {});
bool readPresetFile (const juce::File&, UserPreset&, juce::String& error);

// ---- edit templates (.isfxtemplate): a whole edit stored in beats with media as numbered slots.
// Applying one to a new song re-locks every cut to that song's grid and fills the slots with its takes.
bool saveEditTemplate (const Project&, const Sequence&, const juce::String& name, const juce::File& out, juce::String& error);
bool applyEditTemplate (Project&, const juce::File&, juce::String& error);   // adds a new active sequence
std::vector<juce::File> listEditTemplates (const juce::File& dir = {});
juce::String templateName (const juce::File&);
bool renameEditTemplate (const juce::File&, const juce::String& name, juce::String& error);

// ---- recent projects (newest first, at most 12), kept in userDataDir()/recent.txt
void noteRecentProject (const juce::File&, const juce::File& store = {});
juce::Array<juce::File> recentProjects (const juce::File& store = {});
}
