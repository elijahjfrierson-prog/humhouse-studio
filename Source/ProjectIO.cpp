#include "ProjectIO.h"

namespace hhs
{
static juce::String fileStr (const juce::File& f) { return f == juce::File() ? juce::String() : f.getFullPathName(); }
static juce::File strFile (const juce::String& s) { return s.isEmpty() ? juce::File() : juce::File (s); }

static juce::ValueTree notesTree (const NoteList& notes)
{
    juce::ValueTree t ("NOTES");
    juce::MemoryOutputStream m;
    for (auto& n : notes)
    {
        m.writeInt (n.key); m.writeDouble (n.start); m.writeDouble (n.length); m.writeFloat (n.velocity);
        m.writeFloat (n.pan); m.writeByte ((char) ((n.muted ? 1 : 0) | (n.slide ? 2 : 0)));
    }
    t.setProperty ("data", m.getMemoryBlock().toBase64Encoding(), nullptr);
    return t;
}
static NoteList treeNotes (const juce::ValueTree& t)
{
    NoteList out;
    juce::MemoryBlock mb; mb.fromBase64Encoding (t["data"].toString());
    juce::MemoryInputStream in (mb, false);
    while (in.getNumBytesRemaining() >= 29)
    {
        Note n; n.key = in.readInt(); n.start = in.readDouble(); n.length = in.readDouble(); n.velocity = in.readFloat();
        n.pan = in.readFloat(); const int f = in.readByte(); n.muted = f & 1; n.slide = f & 2;
        out.push_back (n);
    }
    return out;
}

juce::ValueTree projectToTree (const Project& p)
{
    juce::ValueTree root ("HUMHOUSE_PROJECT");
    root.setProperty ("version", 1, nullptr);
    root.setProperty ("title", p.title, nullptr); root.setProperty ("bpm", p.bpm, nullptr);
    root.setProperty ("bpb", p.beatsPerBar, nullptr); root.setProperty ("key", p.keyRoot, nullptr);
    root.setProperty ("scale", p.scaleIndex, nullptr); root.setProperty ("highlight", p.highlightScale, nullptr);
    root.setProperty ("song", p.songMode, nullptr); root.setProperty ("pattern", p.currentPattern, nullptr);
    root.setProperty ("snap", p.snap, nullptr); root.setProperty ("swing", p.swing, nullptr);
    root.setProperty ("punchIn", p.punchIn, nullptr); root.setProperty ("punchOut", p.punchOut, nullptr);
    root.setProperty ("loopRec", p.loopRecord, nullptr); root.setProperty ("rts", p.returnToStart, nullptr);
    root.setProperty ("nextCh", p.nextChannelId, nullptr); root.setProperty ("nextUid", p.nextUid, nullptr);

    juce::ValueTree chs ("CHANNELS");
    for (auto& c : p.channels)
    {
        juce::ValueTree t ("CH");
        t.setProperty ("id", c.id, nullptr); t.setProperty ("name", c.name, nullptr); t.setProperty ("type", (int) c.type, nullptr);
        t.setProperty ("file", fileStr (c.sampleFile), nullptr); t.setProperty ("drum", c.builtinDrum, nullptr);
        t.setProperty ("plugin", c.pluginId, nullptr); t.setProperty ("state", c.pluginState.toBase64Encoding(), nullptr);
        t.setProperty ("mix", c.mixerTrack, nullptr); t.setProperty ("vol", c.volume, nullptr); t.setProperty ("pan", c.pan, nullptr);
        t.setProperty ("pitch", c.pitch, nullptr); t.setProperty ("glide", c.glide, nullptr); t.setProperty ("mono", c.mono, nullptr);
        t.setProperty ("mute", c.muted, nullptr); t.setProperty ("solo", c.solo, nullptr); t.setProperty ("root", c.rootKey, nullptr);
        t.setProperty ("colour", (juce::int64) c.colour.getARGB(), nullptr);
        t.setProperty ("oneShot", c.oneShot, nullptr); t.setProperty ("cutSelf", c.cutItself, nullptr); t.setProperty ("cutGroup", c.cutGroup, nullptr);
        t.setProperty ("ampA", c.ampAttack, nullptr); t.setProperty ("ampH", c.ampHold, nullptr); t.setProperty ("ampR", c.ampRelease, nullptr);
        auto& s = c.synth;
        t.setProperty ("wave", s.wave, nullptr); t.setProperty ("a", s.attack, nullptr); t.setProperty ("d", s.decay, nullptr);
        t.setProperty ("s", s.sustain, nullptr); t.setProperty ("r", s.release, nullptr); t.setProperty ("cut", s.cutoff, nullptr);
        t.setProperty ("res", s.resonance, nullptr); t.setProperty ("det", s.detune, nullptr); t.setProperty ("uni", s.unison, nullptr);
        t.setProperty ("drv", s.drive, nullptr);
        chs.appendChild (t, nullptr);
    }
    root.appendChild (chs, nullptr);

    juce::ValueTree pats ("PATTERNS");
    for (auto& pt : p.patterns)
    {
        juce::ValueTree t ("PAT");
        t.setProperty ("name", pt.name, nullptr); t.setProperty ("colour", (juce::int64) pt.colour.getARGB(), nullptr);
        for (auto& [id, list] : pt.notes) { auto n = notesTree (list); n.setProperty ("ch", id, nullptr); t.appendChild (n, nullptr); }
        pats.appendChild (t, nullptr);
    }
    root.appendChild (pats, nullptr);

    juce::ValueTree lanes ("LANES");
    for (auto& l : p.lanes)
    {
        juce::ValueTree t ("LANE");
        t.setProperty ("name", l.name, nullptr); t.setProperty ("mix", l.mixerTrack, nullptr); t.setProperty ("in", l.inputPair, nullptr);
        t.setProperty ("armed", l.armed, nullptr); t.setProperty ("mute", l.muted, nullptr); t.setProperty ("mon", l.monitor, nullptr);
        t.setProperty ("h", l.height, nullptr);
        lanes.appendChild (t, nullptr);
    }
    root.appendChild (lanes, nullptr);

    juce::ValueTree clips ("CLIPS");
    for (auto& c : p.clips)
    {
        juce::ValueTree t ("CLIP");
        t.setProperty ("uid", c.uid, nullptr); t.setProperty ("lane", c.lane, nullptr); t.setProperty ("start", c.start, nullptr);
        t.setProperty ("len", c.length, nullptr); t.setProperty ("pat", c.patternIndex, nullptr); t.setProperty ("file", fileStr (c.audioFile), nullptr);
        t.setProperty ("off", c.sourceOffset, nullptr); t.setProperty ("gain", c.gain, nullptr); t.setProperty ("mute", c.muted, nullptr);
        t.setProperty ("rev", c.reversed, nullptr); t.setProperty ("mix", c.mixerTrack, nullptr); t.setProperty ("fi", c.fadeIn, nullptr);
        t.setProperty ("fo", c.fadeOut, nullptr); t.setProperty ("fc", c.fadeCurve, nullptr); t.setProperty ("take", c.takeGroup, nullptr);
        t.setProperty ("hidden", c.hidden, nullptr);
        clips.appendChild (t, nullptr);
    }
    root.appendChild (clips, nullptr);

    juce::ValueTree mix ("MIXER");
    for (auto& m : p.mixer)
    {
        juce::ValueTree t ("TRACK");
        t.setProperty ("name", m.name, nullptr); t.setProperty ("vol", m.volume, nullptr); t.setProperty ("pan", m.pan, nullptr);
        t.setProperty ("mute", m.muted, nullptr); t.setProperty ("solo", m.solo, nullptr); t.setProperty ("frozen", m.frozen, nullptr);
        t.setProperty ("frozenFile", fileStr (m.frozenFile), nullptr); t.setProperty ("master", m.routesToMaster, nullptr);
        t.setProperty ("colour", (juce::int64) m.colour.getARGB(), nullptr);
        for (auto& s : m.sends) { juce::ValueTree st ("SEND"); st.setProperty ("to", s.target, nullptr); st.setProperty ("lvl", s.level, nullptr); t.appendChild (st, nullptr); }
        for (auto& f : m.fx)
        {
            juce::ValueTree ft ("FX");
            ft.setProperty ("uid", f.uid, nullptr); ft.setProperty ("type", f.type, nullptr); ft.setProperty ("plugin", f.pluginId, nullptr);
            ft.setProperty ("state", f.pluginState.toBase64Encoding(), nullptr); ft.setProperty ("bypass", f.bypass, nullptr); ft.setProperty ("mix", f.mix, nullptr);
            for (auto& [k, v] : f.params) ft.setProperty (juce::Identifier ("p_" + k), v, nullptr);
            t.appendChild (ft, nullptr);
        }
        mix.appendChild (t, nullptr);
    }
    root.appendChild (mix, nullptr);
    return root;
}

Project projectFromTree (const juce::ValueTree& root)
{
    Project p;
    p.title = root["title"].toString(); p.bpm = root.getProperty ("bpm", 140.0); p.beatsPerBar = root.getProperty ("bpb", 4);
    p.keyRoot = root["key"]; p.scaleIndex = root.getProperty ("scale", 1); p.highlightScale = root.getProperty ("highlight", true);
    p.songMode = root["song"]; p.currentPattern = root["pattern"]; p.snap = root.getProperty ("snap", 0.25); p.swing = root["swing"];
    p.punchIn = root.getProperty ("punchIn", -1.0); p.punchOut = root.getProperty ("punchOut", -1.0);
    p.loopRecord = root.getProperty ("loopRec", true); p.returnToStart = root.getProperty ("rts", true);
    p.nextChannelId = root.getProperty ("nextCh", 1); p.nextUid = root.getProperty ("nextUid", 1);

    for (auto t : root.getChildWithName ("CHANNELS"))
    {
        Channel c;
        c.id = t["id"]; c.name = t["name"].toString(); c.type = (ChannelType) (int) t["type"];
        c.sampleFile = strFile (t["file"].toString()); c.builtinDrum = t.getProperty ("drum", -1);
        c.pluginId = t["plugin"].toString(); c.pluginState.fromBase64Encoding (t["state"].toString());
        c.mixerTrack = t["mix"]; c.volume = t.getProperty ("vol", 0.8f); c.pan = t["pan"]; c.pitch = t["pitch"];
        c.glide = t["glide"]; c.mono = t["mono"]; c.muted = t["mute"]; c.solo = t["solo"]; c.rootKey = t.getProperty ("root", 60);
        c.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0xffff8a3d));
        c.oneShot = t.getProperty ("oneShot", true); c.cutItself = t["cutSelf"]; c.cutGroup = t["cutGroup"];
        c.ampAttack = t["ampA"]; c.ampHold = t["ampH"]; c.ampRelease = t.getProperty ("ampR", 0.01f);
        auto& s = c.synth;
        s.wave = t["wave"]; s.attack = t.getProperty ("a", 0.002f); s.decay = t.getProperty ("d", 0.3f); s.sustain = t.getProperty ("s", 0.7f);
        s.release = t.getProperty ("r", 0.25f); s.cutoff = t.getProperty ("cut", 12000.0f); s.resonance = t.getProperty ("res", 0.2f);
        s.detune = t["det"]; s.unison = t.getProperty ("uni", 1); s.drive = t["drv"];
        p.channels.push_back (c);
    }
    for (auto t : root.getChildWithName ("PATTERNS"))
    {
        Pattern pt; pt.name = t["name"].toString();
        pt.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0xff3d8aff));
        for (auto n : t) pt.notes[(int) n["ch"]] = treeNotes (n);
        p.patterns.push_back (pt);
    }
    for (auto t : root.getChildWithName ("LANES"))
    {
        Lane l; l.name = t["name"].toString(); l.mixerTrack = t["mix"]; l.inputPair = t["in"]; l.armed = t["armed"];
        l.muted = t["mute"]; l.monitor = t.getProperty ("mon", true); l.height = t.getProperty ("h", 56.0f);
        p.lanes.push_back (l);
    }
    for (auto t : root.getChildWithName ("CLIPS"))
    {
        Clip c; c.uid = t["uid"]; c.lane = t["lane"]; c.start = t["start"]; c.length = t["len"]; c.patternIndex = t["pat"];
        c.audioFile = strFile (t["file"].toString()); c.sourceOffset = t["off"]; c.gain = t.getProperty ("gain", 1.0f);
        c.muted = t["mute"]; c.reversed = t["rev"]; c.mixerTrack = t.getProperty ("mix", -1); c.fadeIn = t["fi"]; c.fadeOut = t["fo"];
        c.fadeCurve = t["fc"]; c.takeGroup = t["take"]; c.hidden = t["hidden"];
        p.clips.push_back (c);
    }
    for (auto t : root.getChildWithName ("MIXER"))
    {
        MixerTrack m; m.name = t["name"].toString(); m.volume = t.getProperty ("vol", 0.8f); m.pan = t["pan"];
        m.muted = t["mute"]; m.solo = t["solo"]; m.frozen = t["frozen"]; m.frozenFile = strFile (t["frozenFile"].toString());
        m.routesToMaster = t.getProperty ("master", true);
        m.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0xff2a2f36));
        for (auto c : t)
        {
            if (c.hasType ("SEND")) m.sends.push_back ({ (int) c["to"], (float) c["lvl"] });
            else if (c.hasType ("FX"))
            {
                FxSlot f; f.uid = c["uid"]; f.type = c["type"].toString(); f.pluginId = c["plugin"].toString();
                f.pluginState.fromBase64Encoding (c["state"].toString()); f.bypass = c["bypass"]; f.mix = c.getProperty ("mix", 1.0f);
                for (int i = 0; i < c.getNumProperties(); ++i)
                {
                    auto name = c.getPropertyName (i).toString();
                    if (name.startsWith ("p_")) f.params[name.substring (2)] = (float) c[c.getPropertyName (i)];
                }
                m.fx.push_back (f);
            }
        }
        p.mixer.push_back (m);
    }
    if (p.mixer.empty()) { MixerTrack master; master.name = "Master"; master.routesToMaster = false; p.mixer.push_back (master); }
    return p;
}

bool saveProject (const Project& p, const juce::File& f)
{
    auto xml = projectToTree (p).createXml();
    return xml && xml->writeTo (f);
}

bool loadProject (Project& p, const juce::File& f)
{
    auto xml = juce::XmlDocument::parse (f);
    if (! xml || ! xml->hasTagName ("HUMHOUSE_PROJECT")) return false;
    p = projectFromTree (juce::ValueTree::fromXml (*xml));
    p.projectFile = f;
    return true;
}

juce::File writeBackup (const Project& p, int keep)
{
    auto dir = (p.projectFile.existsAsFile() ? p.projectFile.getParentDirectory()
                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio"))
                   .getChildFile ("Backup");
    dir.createDirectory();
    const auto base = p.projectFile.existsAsFile() ? p.projectFile.getFileNameWithoutExtension() : p.title;
    auto f = dir.getChildFile (base + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".hhs");
    if (! saveProject (p, f)) return {};
    auto all = dir.findChildFiles (juce::File::findFiles, false, base + " *.hhs");
    std::sort (all.begin(), all.end(), [] (auto& a, auto& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    for (int i = keep; i < all.size(); ++i) all.getReference (i).deleteFile();
    return f;
}

void UndoHistory::push (const Project& p, const juce::String& desc)
{
    auto xml = projectToTree (p).toXmlString();
    if (index >= 0 && states[(size_t) index].first == xml) return;
    states.resize ((size_t) (index + 1));
    states.push_back ({ xml, desc });
    if ((int) states.size() > maxLevels + 1) states.erase (states.begin());
    index = (int) states.size() - 1;
}

bool UndoHistory::jumpTo (int i, Project& p)
{
    if (i < 0 || i >= (int) states.size()) return false;
    auto xml = juce::XmlDocument::parse (states[(size_t) i].first);
    if (! xml) return false;
    const auto file = p.projectFile;
    p = projectFromTree (juce::ValueTree::fromXml (*xml));
    p.projectFile = file;
    index = i;
    return true;
}
bool UndoHistory::undo (Project& p) { return canUndo() && jumpTo (index - 1, p); }
bool UndoHistory::redo (Project& p) { return canRedo() && jumpTo (index + 1, p); }
}
