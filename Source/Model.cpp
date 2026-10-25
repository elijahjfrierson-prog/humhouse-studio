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

juce::File bundledSoundsRoot()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/Sounds");
   #else
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("Sounds");
   #endif
}

// starter kit: the bundled Classic Trap (Maajins) sounds, falling back to the built-in drums if the folder isn't installed
static juce::File kitSound (const char* dir, const std::vector<const char*>& prefer)
{
    const auto d = bundledSoundsRoot().getChildFile ("Classic Trap").getChildFile (dir);
    if (! d.isDirectory()) return {};
    for (auto* n : prefer)
        for (auto* ext : { ".flac", ".wav" })
            if (auto f = d.getChildFile (juce::String (n) + ext); f.existsAsFile()) return f;
    auto all = d.findChildFiles (juce::File::findFiles, false, "*.flac;*.wav");
    all.sort();
    return all.isEmpty() ? juce::File() : all[0];
}

int importMidiPattern (Project& p, const juce::File& file)
{
    juce::FileInputStream in (file);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) return -1;
    const short tf = mf.getTimeFormat();
    const double tpq = tf > 0 ? (double) tf : 96.0;
    static const char* roles[] { "Kick", "Clap", "Snare", "Hat", "Open Hat", "Perc" };
    static const char* roleDirs[] { nullptr, "Claps & Snaps", "Snares", "Hi-Hats", "Open Hats", "Percs" };
    auto roleOf = [] (int k) { switch (k) { case 35: case 36: return 0; case 39: return 1; case 38: case 40: return 2;
                                            case 42: case 44: return 3; case 46: return 4; default: return 5; } };
    int ids[6] { -1, -1, -1, -1, -1, -1 };
    auto channelFor = [&] (int role) -> int
    {
        if (ids[role] >= 0) return ids[role];
        const juce::String want (roles[role]);
        for (auto& c : p.channels) if (c.name.equalsIgnoreCase (want)) return ids[role] = c.id;
        for (auto& c : p.channels)
            if (c.name.containsIgnoreCase (want) && (role != 3 || ! c.name.containsIgnoreCase ("open"))) return ids[role] = c.id;
        Channel c; c.name = want; c.mixerTrack = -1;
        c.colour = juce::Colour::fromHSV ((float) role / 8.0f, 0.65f, 0.95f, 1.0f);
        c.builtinDrum = role < 5 ? role : 3;
        if (roleDirs[role] != nullptr)
            if (auto f = kitSound (roleDirs[role], {}); f.existsAsFile()) { c.type = ChannelType::Sampler; c.sampleFile = f; c.builtinDrum = -1; }
        return ids[role] = p.addChannel (c);
    };
    Pattern pat; pat.name = file.getFileNameWithoutExtension(); pat.colour = juce::Colour (0xffff5a8a);
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        for (auto* e : seq)
        {
            if (! e->message.isNoteOn()) continue;
            const int id = channelFor (roleOf (e->message.getNoteNumber()));
            Note n;
            n.start = e->message.getTimeStamp() / tpq;
            n.length = e->noteOffObject != nullptr ? juce::jmax (1.0 / 64.0, (e->noteOffObject->message.getTimeStamp() - e->message.getTimeStamp()) / tpq) : 0.125;
            n.velocity = juce::jlimit (0.05f, 1.0f, e->message.getFloatVelocity());
            for (auto& c : p.channels) if (c.id == id) n.key = c.rootKey;
            pat.notes[id].push_back (n);
        }
    }
    if (pat.notes.empty()) return -1;
    for (auto& [id, nl] : pat.notes) std::sort (nl.begin(), nl.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    p.patterns.push_back (pat);
    return (int) p.patterns.size() - 1;
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
        static const char* kitDirs[] { nullptr, "Claps & Snaps", "Snares", "Hi-Hats", "Open Hats", "808s" };
        static const std::vector<const char*> kitPick[] { {}, { "basic", "smack dvd" }, { "memphis", "realistic" },
                                                                    { "dmv", "PARANOID" }, { "gap", "infinite" }, { "slap mf", "trap mf" } };
        if (kitDirs[i] != nullptr)
            if (auto f = kitSound (kitDirs[i], kitPick[i]); f.existsAsFile())
            {
                c.type = ChannelType::Sampler; c.sampleFile = f; c.builtinDrum = -1;
                if (i == 5) { c.keyTrack = true; c.cutItself = true; c.rootKey = 60; c.glide = 0.08f; c.mono = true; }
            }
        c.colour = juce::Colour::fromHSV ((float) i / 8.0f, 0.65f, 0.95f, 1.0f);
        c.mixerTrack = -1;
        p.addChannel (c);
    }
    Channel keys; keys.name = "Keys"; keys.type = ChannelType::Synth; keys.synth.wave = 0;
    keys.synth.cutoff = 4000.0f; keys.mixerTrack = -1; keys.colour = juce::Colour (0xff6ad1ff);
    p.addChannel (keys);

    Pattern pat; pat.name = "Pattern 1";
    // trap starter: snare + clap on the half (beat 3), sparse kick, 8th hats with 16th sprinkles; no 808 pattern
    for (int s : { 0, 10 }) pat.toggleStep (1, s, 60);
    for (int ch : { 2, 3 }) pat.toggleStep (ch, 8, 60);
    for (int s : { 0, 2, 4, 6, 7, 8, 10, 12, 13, 14 }) pat.toggleStep (4, s, 60);
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
    if (c.mixerTrack < 0)
    {
        // FL: channels fill inserts 1, 2, 3 ... in order. An empty insert a track owns by default is taken over
        // (the track moves to a fresh insert); inserts holding effects or recorded audio are skipped.
        auto usedByChannel = [this] (int i) { for (auto& o : channels) if (o.mixerTrack == i) return true; return false; };
        auto holdsAudio = [this] (int i) { for (auto& cl : clips) if (cl.isAudio() && clipInsert (cl) == i) return true; return false; };
        for (int i = 1; i < (int) mixer.size() && c.mixerTrack < 0; ++i)
        {
            if (usedByChannel (i) || ! mixer[(size_t) i].fx.empty() || holdsAudio (i)) continue;
            bool sentTo = false;
            for (auto& m : mixer) for (auto& snd : m.sends) sentTo |= snd.target == i;
            if (sentTo) continue;
            for (auto& l : lanes) if (l.mixerTrack == i) l.mixerTrack = -1;
            c.mixerTrack = i;
            mixer[(size_t) i].name = c.name;
        }
        if (c.mixerTrack < 0) c.mixerTrack = addMixerTrack (c.name);
        for (auto& l : lanes) if (l.mixerTrack == -1) l.mixerTrack = addMixerTrack (l.name);
    }
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

float autoValueAt (const std::vector<AutoPoint>& pts, double beat)
{
    if (pts.empty()) return 0.5f;
    if (beat <= pts.front().beat) return pts.front().value;
    for (size_t i = 1; i < pts.size(); ++i)
        if (beat < pts[i].beat)
        {
            const auto& a = pts[i - 1]; const auto& b = pts[i];
            const double w = b.beat - a.beat;
            return w > 1.0e-9 ? a.value + (b.value - a.value) * (float) ((beat - a.beat) / w) : b.value;
        }
    return pts.back().value;
}
float autoToValue (int kind, float v)
{
    v = juce::jlimit (0.0f, 1.0f, v);
    switch (kind)
    {
        case AutoInsertVol: return v * 1.25f;
        case AutoChanVol:   return v;
        case AutoInsertPan: case AutoChanPan: return v * 2.0f - 1.0f;
        case AutoChanPitch: return std::round ((v * 2.0f - 1.0f) * 24.0f * 100.0f) / 100.0f;
        default: return v;
    }
}
float autoFromValue (int kind, float x)
{
    switch (kind)
    {
        case AutoInsertVol: return juce::jlimit (0.0f, 1.0f, x / 1.25f);
        case AutoInsertPan: case AutoChanPan: return juce::jlimit (0.0f, 1.0f, (x + 1.0f) * 0.5f);
        case AutoChanPitch: return juce::jlimit (0.0f, 1.0f, (x / 24.0f + 1.0f) * 0.5f);
        default: return juce::jlimit (0.0f, 1.0f, x);
    }
}

double Project::songLengthBeats() const
{
    double end = 0.0;
    for (auto& c : clips) end = std::max (end, c.start + c.length);
    return end;
}

juce::File Project::recordingsFolder() const
{
    if (projectFile.existsAsFile()) return projectFile.getParentDirectory().getChildFile ("Recordings");
    // not saved yet: the song still gets its own session folder (never a shared one); the first save copies it into the song folder
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio").getChildFile ("Sessions")
               .getChildFile (juce::File::createLegalFileName (title.isEmpty() ? juce::String ("Untitled") : title)).getChildFile ("Recordings");
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

int Project::insertPatternAfter (int after, Pattern pt)
{
    const int at = juce::jlimit (0, (int) patterns.size(), after + 1);
    patterns.insert (patterns.begin() + at, std::move (pt));
    for (auto& c : clips) if (c.patternIndex >= at) ++c.patternIndex;
    if (currentPattern >= at) ++currentPattern;
    return at;
}

void Project::removePatterns (const std::function<bool (int)>& drop)
{
    const int n = (int) patterns.size();
    std::vector<int> remap ((size_t) n, -1);
    std::vector<Pattern> kept;
    for (int i = 0; i < n; ++i) if (! drop (i)) { remap[(size_t) i] = (int) kept.size(); kept.push_back (patterns[(size_t) i]); }
    clips.erase (std::remove_if (clips.begin(), clips.end(), [&] (const Clip& c) {
        return c.patternIndex >= 0 && (c.patternIndex >= n || remap[(size_t) c.patternIndex] < 0); }), clips.end());
    for (auto& c : clips) if (c.patternIndex >= 0) c.patternIndex = remap[(size_t) c.patternIndex];
    const int cur = currentPattern >= 0 && currentPattern < n ? remap[(size_t) currentPattern] : -1;
    patterns = std::move (kept);
    if (patterns.empty()) { Pattern pt; pt.name = "Pattern 1"; patterns.push_back (pt); }
    currentPattern = juce::jlimit (0, (int) patterns.size() - 1, cur < 0 ? 0 : cur);
}
}

namespace hhs
{
int rollAudioClips (Project& p, const std::vector<int>& uids, double div, double rangeA, double rangeB)
{
    if (div <= 0) return 0;
    const double fade = secondsToBeats (0.002, p.bpm);
    int g = 0; for (auto& o : p.clips) g = juce::jmax (g, o.group);
    std::vector<Clip> made; int rolled = 0;
    for (int uid : uids)
    {
        auto it = std::find_if (p.clips.begin(), p.clips.end(), [uid] (const Clip& c) { return c.uid == uid; });
        if (it == p.clips.end() || ! it->isAudio()) continue;
        const Clip src = *it;
        const double end = src.start + src.length;
        const bool ranged = rangeB > rangeA;
        const double a = ranged ? juce::jmax (src.start, rangeA) : src.start;
        const double b = ranged ? juce::jmin (end, rangeB) : end;
        if (b - a < div * 1.5 - 1e-9) continue;
        p.clips.erase (it); ++g; ++rolled;
        auto piece = [&] (double s0, double len, double srcBeat)
        {
            Clip c = src; c.uid = p.newUid(); c.start = s0; c.length = len;
            c.sourceOffset = src.sourceOffset + beatsToSeconds (srcBeat - src.start, p.bpm);
            c.group = g; c.takeGroup = 0; c.selected = true;
            return c;
        };
        if (a > src.start + 1e-9)   // audio before the roll stays
        {
            Clip c = piece (src.start, a - src.start, src.start); c.fadeOut = fade; made.push_back (c);
        }
        for (double s0 = a; s0 < b - 1e-9; s0 += div)
        {
            Clip c = piece (s0, juce::jmin (div, b - s0), a);   // every hit replays the audio at the roll start
            c.fadeIn = (s0 == src.start) ? src.fadeIn : fade;
            c.fadeOut = juce::jmin (fade, c.length * 0.5);
            made.push_back (c);
        }
        if (b < end - 1e-9)        // audio after the roll stays
        {
            Clip c = piece (b, end - b, b); c.fadeIn = fade; made.push_back (c);
        }
    }
    p.clips.insert (p.clips.end(), made.begin(), made.end());
    return rolled;
}
}
