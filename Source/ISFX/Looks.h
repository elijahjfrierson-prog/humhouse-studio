#pragma once
#include "Model.h"

namespace isfx
{
struct ParamDef
{
    const char* id; const char* name; float min, max, def; const char* group; const char* help;
};
const std::vector<ParamDef>& sequenceParams();   // grade + FX applied to the whole sequence
const std::vector<ParamDef>& clipParams();       // per-clip transform + grade
const ParamDef* findParam (const std::vector<ParamDef>&, const juce::String& id);
inline float fxValue (const FxStack& s, const std::vector<ParamDef>& defs, const char* id)
{
    auto* d = findParam (defs, id); return s.get (id, d ? d->def : 0.0f);
}

struct Look
{
    juce::String name, category, help;
    std::vector<std::pair<juce::String, float>> p;
};
const std::vector<Look>& lookLibrary();
juce::File contentDir();                                // bundled LUTs/ and SFX/ shipped next to the app
void applyLook (FxStack&, const Look&, float amount);   // blends from defaults

juce::StringArray styleModes();                         // Classic, Gothic, VHS, Y2K, Underground, Abstract
juce::String styleModeHelp (const juce::String&);
void applyStyleMode (FxStack&, const juce::String& mode);

juce::StringArray transitionNames();

// Adobe/Resolve .cube 3D LUT
struct Lut3D
{
    int size = 0;
    std::vector<float> rgb;     // size^3 * 3, red fastest
    juce::String title;
    bool load (const juce::File&, juce::String& error);
};
}
