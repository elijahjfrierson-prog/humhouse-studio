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
    std::map<juce::String, std::vector<Clip::Key>> keys;   // animated params, times relative to the preset start
    Ticks length = 0;                                       // span the animation was authored over
    bool atEnd = false;                                     // OUT presets: keys line up with the clip's end
};
void applyUserPreset (Clip&, const UserPreset&);
// Premiere .prfpset: every preset becomes a clip preset; Premiere effects are rebuilt on native params with their keyframes.
// Effects/params that can't be rebuilt are listed in `missing`. Returns presets written.
int importPrfpset (const juce::File&, const juce::File& outDir, juce::StringArray& missing, juce::String& error);
// Import Preset button: .prfpset, .cube, .isfxpreset, .isfxtemplate (.ffx is kept but needs a native rebuild). Returns a summary.
juce::String importPresetFiles (const juce::Array<juce::File>&, int& imported, const juce::File& root = {});
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
