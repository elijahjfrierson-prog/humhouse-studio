#pragma once
#include "Model.h"

namespace hhs
{
juce::ValueTree projectToTree (const Project&);
Project projectFromTree (const juce::ValueTree&);
bool saveProject (const Project&, const juce::File&);
bool loadProject (Project&, const juce::File&);
juce::File writeBackup (const Project&, int keep = 50);
// one file to email / AirDrop / text: the project as a template, with every sound it uses inside
bool packTemplate (const Project&, const juce::String& name, const juce::File& outPack);
// unpacks a .hhpack into templatesDir/<name>/ and returns the template (.hhs) inside, or {} if it isn't one
juce::File unpackTemplate (const juce::File& pack, const juce::File& templatesDir);   // timestamped copy in <project>/Backup or ~/Music/HumHouse Studio/Backup

// 999-level snapshot undo (FL-style history)
class UndoHistory
{
public:
    static constexpr int maxLevels = 999;
    void push (const Project&, const juce::String& description);
    bool canUndo() const { return index > 0; }
    bool canRedo() const { return index + 1 < (int) states.size(); }
    bool undo (Project&);
    bool redo (Project&);
    bool jumpTo (int i, Project&);
    int  size() const { return (int) states.size(); }
    int  current() const { return index; }
    juce::String description (int i) const { return states[(size_t) i].second; }
    void clear() { states.clear(); index = -1; }
private:
    std::vector<std::pair<juce::String, juce::String>> states; // xml, description
    int index = -1;
};
}
