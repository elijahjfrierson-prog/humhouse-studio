#include "Model.h"
#include <algorithm>
#include <set>
#include <functional>

namespace hhs
{
const std::vector<ScaleDef>& allScales()
{
    static const std::vector<ScaleDef> s {
        { "Major",            { 0, 2, 4, 5, 7, 9, 11 } },
        { "Natural Minor",    { 0, 2, 3, 5, 7, 8, 10 } },
        { "Harmonic Minor",   { 0, 2, 3, 5, 7, 8, 11 } },
        { "Melodic Minor",    { 0, 2, 3, 5, 7, 9, 11 } },
        { "Dorian",           { 0, 2, 3, 5, 7, 9, 10 } },
        { "Phrygian",         { 0, 1, 3, 5, 7, 8, 10 } },
        { "Lydian",           { 0, 2, 4, 6, 7, 9, 11 } },
        { "Mixolydian",       { 0, 2, 4, 5, 7, 9, 10 } },
        { "Locrian",          { 0, 1, 3, 5, 6, 8, 10 } },
        { "Major Pentatonic", { 0, 2, 4, 7, 9 } },
        { "Minor Pentatonic", { 0, 3, 5, 7, 10 } },
        { "Blues",            { 0, 3, 5, 6, 7, 10 } },
        { "Hungarian Minor",  { 0, 2, 3, 6, 7, 8, 11 } },
        { "Phrygian Dominant",{ 0, 1, 4, 5, 7, 8, 10 } },
        { "Whole Tone",       { 0, 2, 4, 6, 8, 10 } },
        { "Chromatic",        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    };
    return s;
}

const char* pitchClassName (int pc)
{
    static const char* n[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n[((pc % 12) + 12) % 12];
}

const char* noteName (int key)
{
    static thread_local char buf[8];
    snprintf (buf, sizeof (buf), "%s%d", pitchClassName (key), key / 12);   // FL naming: middle C (60) = C5
    return buf;
}

bool inScale (int key, int root, int scaleIndex)
{
    const auto& sc = allScales()[(size_t) juce::jlimit (0, (int) allScales().size() - 1, scaleIndex)].steps;
    const int pc = (((key - root) % 12) + 12) % 12;
    return std::find (sc.begin(), sc.end(), pc) != sc.end();
}

int snapToScale (int key, int root, int scaleIndex)
{
    for (int d = 0; d < 12; ++d)
    {
        if (inScale (key - d, root, scaleIndex)) return key - d;
        if (inScale (key + d, root, scaleIndex)) return key + d;
    }
    return key;
}

double beatsToSeconds (double beats, double bpm) { return beats * 60.0 / bpm; }
double secondsToBeats (double secs, double bpm)  { return secs * bpm / 60.0; }

// ---------------------------------------------------------------- pattern
double Pattern::lengthBeats() const
{
    double end = 0.0;
    for (auto& [id, list] : notes)
        for (auto& n : list) end = std::max (end, n.end());
    return std::max (4.0, std::ceil (end / 4.0 - 1.0e-9) * 4.0);
}

bool Pattern::stepOn (int channelId, int step, double stepLen) const
{
    auto it = notes.find (channelId);
    if (it == notes.end()) return false;
    const double t = step * stepLen;
    for (auto& n : it->second)
        if (std::abs (n.start - t) < 1.0e-6) return true;
    return false;
}

void Pattern::toggleStep (int channelId, int step, int rootKey, double stepLen)
{
    auto& list = notes[channelId];
    const double t = step * stepLen;
    for (auto it = list.begin(); it != list.end(); ++it)
        if (std::abs (it->start - t) < 1.0e-6) { list.erase (it); return; }
    Note n; n.key = rootKey; n.start = t; n.length = stepLen; n.velocity = 0.78f;
    list.push_back (n);
}

// ---------------------------------------------------------------- project
Project Project::createDefault()
{
    Project p;
    MixerTrack master; master.name = "Master"; master.routesToMaster = false;
    p.mixer.push_back (master);
    Pattern pat; pat.name = "Pattern 1";
    p.patterns.push_back (pat);
    for (int i = 0; i < 10; ++i)
    {
        Lane l; l.name = "Track " + juce::String (i + 1);
        p.lanes.push_back (l);
    }
    for (int i = 0; i < 10; ++i) p.lanes[(size_t) i].mixerTrack = p.addMixerTrack ("Track " + juce::String (i + 1));
    Clip c; c.uid = p.newUid(); c.lane = 0; c.start = 0; c.length = 4; c.patternIndex = 0;
    p.clips.push_back (c);
    return p;
}

Project Project::createDemo()
{
    Project p;
    MixerTrack master; master.name = "Master"; master.routesToMaster = false;
    p.mixer.push_back (master);

    static const char* drumNames[] { "Kick", "Clap", "Snare", "Hat", "Open Hat", "808" };
    for (int i = 0; i < 6; ++i)
    {
        Channel c;
        c.name = drumNames[i];
        if (i == 5) { c.type = ChannelType::Synth; c.synth.wave = 4; c.glide = 0.08f; c.mono = true; c.rootKey = 36;
                      c.synth.attack = 0.001f; c.synth.decay = 1.2f; c.synth.sustain = 0.6f; c.synth.release = 0.3f; c.synth.drive = 0.25f; }
        else        { c.builtinDrum = i; }
        c.colour = juce::Colour::fromHSV ((float) i / 8.0f, 0.65f, 0.95f, 1.0f);
        c.mixerTrack = -1;
        p.addChannel (c);
    }
    Channel keys; keys.name = "Keys"; keys.type = ChannelType::Synth; keys.synth.wave = 0;
    keys.synth.cutoff = 4000.0f; keys.mixerTrack = -1; keys.colour = juce::Colour (0xff6ad1ff);
    p.addChannel (keys);

    Pattern pat; pat.name = "Pattern 1";
    // starter groove: kick on 1 & 3, clap on 2 & 4, 8th hats
    for (int s : { 0, 8 }) pat.toggleStep (1, s, 60);
    for (int s : { 4, 12 }) pat.toggleStep (2, s, 60);
    for (int s = 0; s < 16; s += 2) pat.toggleStep (4, s, 60);
    p.patterns.push_back (pat);

    for (int i = 0; i < 8; ++i)
    {
        Lane l; l.name = i == 0 ? "Beat" : (i == 1 ? "Vocal" : "Track " + juce::String (i + 1));
        p.lanes.push_back (l);
    }
    p.lanes[1].mixerTrack = p.addMixerTrack ("Vocal");
    Clip c; c.uid = p.newUid(); c.lane = 0; c.start = 0; c.length = 4; c.patternIndex = 0;
    p.clips.push_back (c);
    return p;
}

Channel* Project::findChannel (int id)
{
    for (auto& c : channels) if (c.id == id) return &c;
    return nullptr;
}
const Channel* Project::findChannel (int id) const
{
    for (auto& c : channels) if (c.id == id) return &c;
    return nullptr;
}

int Project::addChannel (Channel c)
{
    c.id = nextChannelId++;
    if (c.mixerTrack < 0) c.mixerTrack = addMixerTrack (c.name);
    channels.push_back (c);
    return c.id;
}

FxSlot& Project::addFx (int track, const juce::String& type)
{
    FxSlot s; s.uid = newUid(); s.type = type;
    auto& fx = mixer[(size_t) track].fx;
    fx.push_back (s);
    return fx.back();
}

int Project::addMixerTrack (const juce::String& name)
{
    MixerTrack t; t.name = name;
    mixer.push_back (t);
    return (int) mixer.size() - 1;
}

int Project::clipInsert (const Clip& c) const
{
    if (c.mixerTrack >= 0 && c.mixerTrack < (int) mixer.size()) return c.mixerTrack;
    if (c.lane >= 0 && c.lane < (int) lanes.size()) return lanes[(size_t) c.lane].mixerTrack;
    return 0;
}

double Project::songLengthBeats() const
{
    double end = 0.0;
    for (auto& c : clips) end = std::max (end, c.start + c.length);
    return end;
}

juce::File Project::recordingsFolder() const
{
    auto base = projectFile.existsAsFile() ? projectFile.getParentDirectory()
                                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio");
    return base.getChildFile ("Recordings");
}

std::vector<int> Project::outputsOf (int track) const
{
    std::vector<int> out;
    if (track <= 0 || track >= (int) mixer.size()) return out;
    auto& t = mixer[(size_t) track];
    for (auto& s : t.sends) if (s.target >= 0 && s.target < (int) mixer.size()) out.push_back (s.target);
    if (t.routesToMaster) out.push_back (0);
    return out;
}

bool Project::wouldCreateCycle (int from, int to) const
{
    if (from == to) return true;
    // cycle if 'from' is reachable from 'to'
    std::set<int> seen;
    std::function<bool (int)> reach = [&] (int n)
    {
        if (n == from) return true;
        if (! seen.insert (n).second) return false;
        for (int o : outputsOf (n)) if (reach (o)) return true;
        return false;
    };
    return reach (to);
}

bool Project::toggleRoute (int from, int to)
{
    if (from <= 0 || from >= (int) mixer.size() || to < 0 || to >= (int) mixer.size()) return false;
    auto& t = mixer[(size_t) from];
    if (to == 0) { t.routesToMaster = ! t.routesToMaster; return true; }
    for (auto it = t.sends.begin(); it != t.sends.end(); ++it)
        if (it->target == to) { t.sends.erase (it); return true; }
    if (wouldCreateCycle (from, to)) return false;
    t.sends.push_back ({ to, 1.0f });
    return true;
}

std::vector<std::vector<int>> Project::processingLevels() const
{
    // level(n) = 1 + max(level of inputs). Tracks in one level do not feed each other.
    const int n = (int) mixer.size();
    std::vector<int> level ((size_t) n, 0);
    for (int pass = 0; pass < n; ++pass)
    {
        bool changed = false;
        for (int t = 1; t < n; ++t)
            for (int o : outputsOf (t))
                if (level[(size_t) o] < level[(size_t) t] + 1) { level[(size_t) o] = level[(size_t) t] + 1; changed = true; }
        if (! changed) break;
    }
    int maxL = 0;
    for (int t = 1; t < n; ++t) maxL = std::max (maxL, level[(size_t) t]);
    std::vector<std::vector<int>> levels ((size_t) maxL + 1);
    for (int t = 1; t < n; ++t) levels[(size_t) level[(size_t) t]].push_back (t);
    levels.push_back ({ 0 });
    return levels;
}

std::vector<int> Project::processingOrder() const
{
    std::vector<int> order;
    for (auto& l : processingLevels()) order.insert (order.end(), l.begin(), l.end());
    return order;
}
}
