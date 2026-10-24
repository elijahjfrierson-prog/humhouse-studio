#include "ProjectIO.h"
#include <map>
#include "Drummer/DrummerCore.h"

namespace hhs
{
// the folder of the .hhs being saved / opened: audio inside it is also stored relative, so a moved,
// copied or synced project folder (Desktop, USB drive, other computer) still finds its vocals
static thread_local juce::File ioBase;
// paths are stored relative to the song folder; a backup (song/Backup/x.hhs) points at the same song folder, so it keeps its audio
struct IOBaseScope
{
    explicit IOBaseScope (const juce::File& projectFile)
    {
        ioBase = projectFile.getParentDirectory();
        if (ioBase.getFileName() == "Backup") ioBase = ioBase.getParentDirectory();
    }
    ~IOBaseScope() { ioBase = juce::File(); }
};

static juce::String fileStr (const juce::File& f) { return f == juce::File() ? juce::String() : f.getFullPathName(); }
static juce::String relStr (const juce::File& f)
{
    if (f == juce::File() || ioBase == juce::File() || ! f.isAChildOf (ioBase)) return {};
    return f.getRelativePathFrom (ioBase).replaceCharacter ('\\', '/');
}
static void setFile (juce::ValueTree& t, const char* key, const juce::File& f)
{
    t.setProperty (key, fileStr (f), nullptr);
    const auto rel = relStr (f);
    if (rel.isNotEmpty()) t.setProperty (juce::String (key) + "Rel", rel, nullptr);
    if (f.existsAsFile()) t.setProperty (juce::String (key) + "Size", (juce::int64) f.getSize(), nullptr);
}
// size = the file's size when saved (0 = unknown, older projects): a file found only by its name must match it,
// so a song never relinks to another song's take that happens to share the name
static juce::File findAudio (const juce::String& s, const juce::String& rel, juce::int64 size = 0)
{
    if (s.isEmpty()) return {};
    juce::File f (s);
    if (f.existsAsFile()) return f;
    auto alt0 = [] (const juce::File& c) { return c.existsAsFile() ? c : (c.hasFileExtension ("wav") && c.withFileExtension ("flac").existsAsFile()) ? c.withFileExtension ("flac") : juce::File(); };
    if (auto c = alt0 (f); c != juce::File()) return c;   // built-in kits are FLAC now
    if (rel.isNotEmpty() && ioBase != juce::File()) if (auto c = alt0 (ioBase.getChildFile (rel)); c != juce::File()) return c;
    auto alt = [&] (const juce::File& c) { auto a = alt0 (c); return a != juce::File() && (size <= 0 || a.hasFileExtension ("flac") || a.getSize() == size) ? a : juce::File(); };
    const auto name = s.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
    juce::Array<juce::File> tries;
    if (ioBase != juce::File())
    {
        if (rel.isNotEmpty()) tries.add (ioBase.getChildFile (rel));
        tries.add (ioBase.getChildFile ("Recordings").getChildFile (name));
        tries.add (ioBase.getChildFile (name));
        tries.add (ioBase.getChildFile ("Audio Files").getChildFile (name));
        tries.add (ioBase.getChildFile ("Samples").getChildFile (name));
        tries.add (ioBase.getChildFile ("Audio").getChildFile (name));
        tries.add (ioBase.getParentDirectory().getChildFile ("Recordings").getChildFile (name));
    }
    for (auto root : { juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio"),
                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("HumHouse Studio") })
    {
        tries.add (root.getChildFile ("Recordings").getChildFile (name));
        tries.add (root.getChildFile ("Templates").getChildFile ("Recordings").getChildFile (name));   // older builds recorded template-based songs here
        tries.add (root.getChildFile ("Audio Files").getChildFile (name));
    }
    if (ioBase != juce::File()) tries.add (ioBase.getChildFile ("Templates").getChildFile ("Recordings").getChildFile (name));
    for (auto& c : tries) if (auto a = alt (c); a != juce::File()) return a;
    if (ioBase.isDirectory())   // last try: anywhere inside the project folder (bounded so a Desktop full of files can't stall opening)
    {
        int seen = 0;
        for (const auto& e : juce::RangedDirectoryIterator (ioBase, true, "*", juce::File::findFiles))
        {
            if (++seen > 20000) break;
            if (e.getFile().getFileName().equalsIgnoreCase (name) && (size <= 0 || e.getFile().getSize() == size)) return e.getFile();
        }
    }
    return f;   // still missing: keep the original path so it can be located later
}
static juce::File readFile (const juce::ValueTree& t, const char* key) { return findAudio (t[key].toString(), t[juce::Identifier (juce::String (key) + "Rel")].toString(), (juce::int64) t.getProperty (juce::Identifier (juce::String (key) + "Size"), (juce::int64) 0)); }

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
    root.setProperty ("version", 2, nullptr);
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
        setFile (t, "file", c.sampleFile); t.setProperty ("drum", c.builtinDrum, nullptr);
        if (c.type == ChannelType::Drummer)
        {
            auto& d = c.drum;
            const std::pair<const char*, float> fs[] { { "drC", d.complexity }, { "drL", d.loudness }, { "drI", d.intensity }, { "drF", d.fills }, { "drSw", d.swing },
                { "drHu", d.humanize }, { "drGh", d.ghost }, { "drHo", d.hatOpen }, { "drPu", d.punch }, { "drGu", d.glue }, { "drDr", d.drive },
                { "drMb", d.micBlend }, { "drRm", d.roomMix }, { "drSq", d.squeeze } };
            for (auto& [k, v] : fs) t.setProperty (k, v, nullptr);
            t.setProperty ("drHt", d.halfTime, nullptr); t.setProperty ("drRi", d.ride, nullptr); t.setProperty ("drCh", d.character, nullptr);
            t.setProperty ("drSe", d.seed, nullptr); t.setProperty ("drVo", d.voicing, nullptr); t.setProperty ("drSp", d.space, nullptr); t.setProperty ("drGw", d.glow, nullptr);
            for (auto [k, v] : { std::pair { "drKp", d.kickPattern }, { "drSnp", d.snarePattern }, { "drHp", d.hatPattern }, { "drTp", d.tomPattern }, { "drCp", d.crashPattern } })
                t.setProperty (k, v, nullptr);
            {
                juce::StringArray pc, sm;
                for (int g = 0; g < 5; ++g) for (auto* a : { &d.pieceDb, &d.piecePan, &d.pieceTune, &d.pieceDamp }) pc.add (juce::String ((*a)[(size_t) g], 3));
                for (int m : d.sectionMute) sm.add (juce::String (m));
                juce::StringArray pd; for (float v : d.partDb) pd.add (juce::String (v, 3));
                t.setProperty ("drPd", pd.joinIntoString (","), nullptr);
                juce::StringArray sv; for (int v : d.sectionVel) sv.add (juce::String (v));
                t.setProperty ("drSV", sv.joinIntoString (","), nullptr);
                t.setProperty ("drGM", d.groupMask, nullptr); t.setProperty ("drLk", c.drumLink, nullptr); t.setProperty ("mLk", c.midiLink, nullptr);
                juce::StringArray ms;
                for (auto& [k, a] : d.steps) ms.add (juce::String (k) + ":" + juce::String (a[0]) + "." + juce::String (a[1]) + "." + juce::String (a[2]) + "." + juce::String (a[3]) + "." + juce::String (a[4]));
                t.setProperty ("drMS", ms.joinIntoString (";"), nullptr);
                t.setProperty ("drPc", pc.joinIntoString (","), nullptr); t.setProperty ("drSM", sm.joinIntoString (","), nullptr);
            }
        }
        t.setProperty ("plugin", c.pluginId, nullptr); t.setProperty ("state", c.pluginState.toBase64Encoding(), nullptr);
        t.setProperty ("mix", c.mixerTrack, nullptr); t.setProperty ("vol", c.volume, nullptr); t.setProperty ("pan", c.pan, nullptr);
        t.setProperty ("pitch", c.pitch, nullptr); t.setProperty ("glide", c.glide, nullptr); t.setProperty ("shift", c.shift, nullptr); t.setProperty ("mono", c.mono, nullptr);
        t.setProperty ("mute", c.muted, nullptr); t.setProperty ("solo", c.solo, nullptr); t.setProperty ("root", c.rootKey, nullptr);
        t.setProperty ("colour", (juce::int64) c.colour.getARGB(), nullptr);
        t.setProperty ("oneShot", c.oneShot, nullptr); t.setProperty ("cutSelf", c.cutItself, nullptr); t.setProperty ("cutGroup", c.cutGroup, nullptr);
        t.setProperty ("ampA", c.ampAttack, nullptr); t.setProperty ("ampH", c.ampHold, nullptr); t.setProperty ("ampR", c.ampRelease, nullptr);
        t.setProperty ("smpNorm", c.smpNormalize, nullptr); t.setProperty ("smpRev", c.smpReverse, nullptr); t.setProperty ("smpInv", c.smpInvert, nullptr);
        t.setProperty ("smpDeclick", c.smpDeclick, nullptr); t.setProperty ("keyTrack", c.keyTrack, nullptr); t.setProperty ("smpTrim", c.smpTrim, nullptr); t.setProperty ("smpBoost", c.smpBoostDb, nullptr);
        t.setProperty ("smpFadeIn", c.smpFadeIn, nullptr); t.setProperty ("smpFadeOut", c.smpFadeOut, nullptr);
        t.setProperty ("stretchMode", c.stretchMode, nullptr); t.setProperty ("stretchTime", c.stretchTime, nullptr);
        t.setProperty ("tempoSync", c.tempoSync, nullptr); t.setProperty ("sampleBpm", c.sampleBpm, nullptr);
        auto& s = c.synth;
        t.setProperty ("wave", s.wave, nullptr); t.setProperty ("a", s.attack, nullptr); t.setProperty ("d", s.decay, nullptr);
        t.setProperty ("s", s.sustain, nullptr); t.setProperty ("r", s.release, nullptr); t.setProperty ("cut", s.cutoff, nullptr);
        t.setProperty ("res", s.resonance, nullptr); t.setProperty ("det", s.detune, nullptr); t.setProperty ("uni", s.unison, nullptr);
        t.setProperty ("drv", s.drive, nullptr);
        t.setProperty ("fmr", s.fmRatio, nullptr); t.setProperty ("fmi", s.fmIndex, nullptr); t.setProperty ("mor", s.morph, nullptr);
        t.setProperty ("noi", s.noise, nullptr); t.setProperty ("lfr", s.lfoRate, nullptr); t.setProperty ("vib", s.vibrato, nullptr);
        {
            juce::StringArray x;
            for (size_t o = 0; o < 3; ++o) { x.add (juce::String (s.xWave[o])); x.add (juce::String (s.xVol[o])); x.add (juce::String (s.xSemi[o])); x.add (juce::String (s.xFine[o])); }
            t.setProperty ("x3", x.joinIntoString (","), nullptr);
            t.setProperty ("wtPos", s.wtPos, nullptr); t.setProperty ("wtKind", s.wtKind, nullptr);
            if (s.wt != nullptr && ! s.wt->empty())
            {
                t.setProperty ("wt", juce::MemoryBlock (s.wt->data(), s.wt->size() * sizeof (float)).toBase64Encoding(), nullptr);
                t.setProperty ("wtName", s.wtName, nullptr);
            }
        }
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
        t.setProperty ("name", l.name, nullptr); t.setProperty ("mix", l.mixerTrack, nullptr); t.setProperty ("in", l.inputPair, nullptr); t.setProperty ("inc", l.inputCh, nullptr); t.setProperty ("ist", l.stereoIn, nullptr); t.setProperty ("norm", l.autoNorm, nullptr); t.setProperty ("shiftms", l.shiftMs, nullptr); t.setProperty ("comp", l.compLane, nullptr); t.setProperty ("lmute", l.muted, nullptr); t.setProperty ("colour", (juce::int64) l.colour.getARGB(), nullptr); t.setProperty ("ncol", (juce::int64) l.nameColour.getARGB(), nullptr); t.setProperty ("nfont", l.nameFont, nullptr);
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
        t.setProperty ("len", c.length, nullptr); t.setProperty ("pat", c.patternIndex, nullptr); t.setProperty ("poff", c.patOffset, nullptr); setFile (t, "file", c.audioFile);
        t.setProperty ("off", c.sourceOffset, nullptr); t.setProperty ("gain", c.gain, nullptr); t.setProperty ("mute", c.muted, nullptr);
        t.setProperty ("rev", c.reversed, nullptr); t.setProperty ("mix", c.mixerTrack, nullptr); t.setProperty ("fi", c.fadeIn, nullptr);
        t.setProperty ("fo", c.fadeOut, nullptr); t.setProperty ("fc", c.fadeCurve, nullptr); t.setProperty ("take", c.takeGroup, nullptr); t.setProperty ("grp", c.group, nullptr);
        if (c.drummer >= 0) t.setProperty ("drummer", c.drummer, nullptr);
        if (c.isAuto())
        {
            auto& at = c.autoTarget;
            t.setProperty ("ak", at.kind, nullptr); t.setProperty ("at", at.track, nullptr); t.setProperty ("afx", at.fxUid, nullptr);
            t.setProperty ("ach", at.channel, nullptr); t.setProperty ("ap", at.param, nullptr); t.setProperty ("an", at.name, nullptr);
            for (auto& pt : c.autoPts) { juce::ValueTree q ("PT"); q.setProperty ("b", pt.beat, nullptr); q.setProperty ("v", pt.value, nullptr); t.appendChild (q, nullptr); }
        }
        t.setProperty ("hidden", c.hidden, nullptr);
        if (! c.colour.isTransparent()) t.setProperty ("colour", (juce::int64) c.colour.getARGB(), nullptr);
        clips.appendChild (t, nullptr);
    }
    root.appendChild (clips, nullptr);
    juce::ValueTree pool ("POOL");
    for (auto& f : p.audioPool) { juce::ValueTree t ("F"); setFile (t, "file", f); pool.appendChild (t, nullptr); }
    root.appendChild (pool, nullptr);

    juce::ValueTree mix ("MIXER");
    for (auto& m : p.mixer)
    {
        juce::ValueTree t ("TRACK");
        t.setProperty ("name", m.name, nullptr); t.setProperty ("vol", m.volume, nullptr); t.setProperty ("pan", m.pan, nullptr);
        t.setProperty ("mute", m.muted, nullptr); t.setProperty ("solo", m.solo, nullptr); t.setProperty ("frozen", m.frozen, nullptr);
        setFile (t, "frozenFile", m.frozenFile); t.setProperty ("master", m.routesToMaster, nullptr);
        t.setProperty ("duck", m.duck, nullptr); t.setProperty ("duckSrc", m.duckSource, nullptr); t.setProperty ("duckTh", m.duckThreshold, nullptr);
        t.setProperty ("duckRatio", m.duckRatio, nullptr); t.setProperty ("duckAtt", m.duckAttack, nullptr); t.setProperty ("duckRel", m.duckRelease, nullptr);
        t.setProperty ("colour", (juce::int64) m.colour.getARGB(), nullptr);
        {
            juce::StringArray q;
            for (size_t b = 0; b < 5; ++b) { q.add (juce::String (m.meqFreq[b])); q.add (juce::String (m.meqGain[b])); q.add (juce::String (m.meqQ[b])); }
            t.setProperty ("meq", q.joinIntoString (","), nullptr);
        }
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
    juce::ValueTree links ("CTLLINKS");
    for (auto& L : p.ctlLinks)
    {
        juce::ValueTree t ("L");
        t.setProperty ("st", L.srcTrack, nullptr); t.setProperty ("su", L.srcUid, nullptr); t.setProperty ("dt", L.dstTrack, nullptr);
        t.setProperty ("du", L.dstUid, nullptr); t.setProperty ("par", L.param, nullptr); t.setProperty ("lo", L.lo, nullptr); t.setProperty ("hi", L.hi, nullptr);
        links.appendChild (t, nullptr);
    }
    root.appendChild (links, nullptr);
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
        c.sampleFile = readFile (t, "file"); c.builtinDrum = t.getProperty ("drum", -1);
        if (c.type == ChannelType::Drummer)
        {
            auto& d = c.drum;
            const std::pair<const char*, float*> fs[] { { "drC", &d.complexity }, { "drL", &d.loudness }, { "drI", &d.intensity }, { "drF", &d.fills }, { "drSw", &d.swing },
                { "drHu", &d.humanize }, { "drGh", &d.ghost }, { "drHo", &d.hatOpen }, { "drPu", &d.punch }, { "drGu", &d.glue }, { "drDr", &d.drive },
                { "drMb", &d.micBlend }, { "drRm", &d.roomMix }, { "drSq", &d.squeeze } };
            for (auto& [k, v] : fs) *v = (float) t.getProperty (k, *v);
            d.halfTime = t.getProperty ("drHt", d.halfTime); d.ride = t.getProperty ("drRi", d.ride); d.character = t.getProperty ("drCh", d.character);
            d.seed = t.getProperty ("drSe", d.seed); d.voicing = t.getProperty ("drVo", d.voicing); d.space = t.getProperty ("drSp", d.space); d.glow = t.getProperty ("drGw", d.glow);
            for (auto [k, v] : { std::pair { "drKp", &d.kickPattern }, { "drSnp", &d.snarePattern }, { "drHp", &d.hatPattern }, { "drTp", &d.tomPattern }, { "drCp", &d.crashPattern } })
                *v = t.getProperty (k, *v);
            {
                const auto pc = juce::StringArray::fromTokens (t["drPc"].toString(), ",", "");
                if (pc.size() == 20)
                    for (int g = 0, k = 0; g < 5; ++g) for (auto* a : { &d.pieceDb, &d.piecePan, &d.pieceTune, &d.pieceDamp }) (*a)[(size_t) g] = pc[k++].getFloatValue();
                const auto pd = juce::StringArray::fromTokens (t["drPd"].toString(), ",", "");
                if (pd.size() == (int) d.partDb.size())
                    for (int k = 0; k < pd.size(); ++k) d.partDb[(size_t) k] = pd[k].getFloatValue();
                d.sectionMute.clear();
                for (auto& v : juce::StringArray::fromTokens (t["drSM"].toString(), ",", "")) if (v.isNotEmpty()) d.sectionMute.push_back (v.getIntValue());
                d.sectionVel.clear();
                for (auto& v : juce::StringArray::fromTokens (t["drSV"].toString(), ",", "")) if (v.isNotEmpty()) d.sectionVel.push_back (v.getIntValue());
                d.groupMask = (int) t.getProperty ("drGM", 31); c.drumLink = (int) t.getProperty ("drLk", 0); c.midiLink = (bool) t.getProperty ("mLk", false);
                d.steps.clear();
                for (auto& e : juce::StringArray::fromTokens (t["drMS"].toString(), ";", ""))
                {
                    const auto kv = juce::StringArray::fromTokens (e, ":", ""), v = juce::StringArray::fromTokens (kv[1], ".", "");
                    if (kv.size() != 2 || v.size() != 5) continue;
                    std::array<int, 5> a {};
                    for (int g = 0; g < 5; ++g) a[(size_t) g] = v[g].getIntValue();
                    d.steps[kv[0].getIntValue()] = a;
                }
            }
            if (! c.sampleFile.isDirectory()) { const auto kit = drumKits(); for (auto& k : kit) if (k.getFileName() == c.sampleFile.getFileName()) c.sampleFile = k; }
            if (! c.sampleFile.isDirectory()) c.sampleFile = defaultDrumKit();   // an older install path: use this install's kit
        }
        c.pluginId = t["plugin"].toString(); c.pluginState.fromBase64Encoding (t["state"].toString());
        c.mixerTrack = t["mix"]; c.volume = t.getProperty ("vol", 0.8f); c.pan = t["pan"]; c.pitch = t["pitch"];
        c.glide = t["glide"]; c.shift = (float) t.getProperty ("shift", 0.0); c.mono = t["mono"]; c.muted = t["mute"]; c.solo = t["solo"]; c.rootKey = t.getProperty ("root", 60);
        c.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0xffff8a3d));
        c.oneShot = t.getProperty ("oneShot", true); c.cutItself = t["cutSelf"]; c.cutGroup = t["cutGroup"];
        c.ampAttack = t["ampA"]; c.ampHold = t["ampH"]; c.ampRelease = t.getProperty ("ampR", 0.01f);
        c.smpNormalize = t["smpNorm"]; c.smpReverse = t["smpRev"]; c.smpInvert = t["smpInv"]; c.smpDeclick = t.getProperty ("smpDeclick", true); c.keyTrack = t.getProperty ("keyTrack", true);
        c.smpTrim = t["smpTrim"]; c.smpBoostDb = t["smpBoost"]; c.smpFadeIn = t["smpFadeIn"]; c.smpFadeOut = t["smpFadeOut"];
        c.stretchMode = t["stretchMode"]; c.stretchTime = t.getProperty ("stretchTime", 1.0f);
        c.tempoSync = t.getProperty ("tempoSync", true); c.sampleBpm = t.getProperty ("sampleBpm", 0.0f);
        // v1 projects turned KEY off for every non-808 sample, so Piano Roll notes all played the same pitch
        if ((int) root.getProperty ("version", 1) < 2 && c.type == ChannelType::Sampler && ! c.keyTrack
            && ! isDrumSampleName (c.sampleFile.getFileNameWithoutExtension()))
            c.keyTrack = true;
        auto& s = c.synth;
        s.wave = t["wave"]; s.attack = t.getProperty ("a", 0.002f); s.decay = t.getProperty ("d", 0.3f); s.sustain = t.getProperty ("s", 0.7f);
        s.release = t.getProperty ("r", 0.25f); s.cutoff = t.getProperty ("cut", 12000.0f); s.resonance = t.getProperty ("res", 0.2f);
        s.detune = t["det"]; s.unison = t.getProperty ("uni", 1); s.drive = t["drv"];
        s.fmRatio = t.getProperty ("fmr", 1.0f); s.fmIndex = t["fmi"]; s.morph = t["mor"];
        s.noise = t["noi"]; s.lfoRate = t["lfr"]; s.vibrato = t["vib"];
        {
            const auto x = juce::StringArray::fromTokens (t["x3"].toString(), ",", "");
            if (x.size() == 12)
                for (size_t o = 0; o < 3; ++o)
                {
                    s.xWave[o] = x[(int) o * 4].getIntValue(); s.xVol[o] = x[(int) o * 4 + 1].getFloatValue();
                    s.xSemi[o] = x[(int) o * 4 + 2].getFloatValue(); s.xFine[o] = x[(int) o * 4 + 3].getFloatValue();
                }
            s.wtPos = t["wtPos"]; s.wtKind = t["wtKind"];
            juce::MemoryBlock mb;
            if (t.hasProperty ("wt") && mb.fromBase64Encoding (t["wt"].toString()) && mb.getSize() >= humWaveFrameSize * sizeof (float))
            {
                std::vector<float> v (mb.getSize() / sizeof (float));
                std::memcpy (v.data(), mb.getData(), v.size() * sizeof (float));
                s.wt = std::make_shared<const std::vector<float>> (std::move (v));
                s.wtName = t["wtName"].toString();
            }
        }
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
        Lane l; l.name = t["name"].toString(); l.mixerTrack = t["mix"]; l.inputPair = t["in"]; l.inputCh = (int) t.getProperty ("inc", (int) t["in"] * 2); l.stereoIn = t.getProperty ("ist", false); l.autoNorm = t.getProperty ("norm", false); l.shiftMs = (float) t.getProperty ("shiftms", 0.0); l.compLane = t.getProperty ("comp", -1); l.muted = t.getProperty ("lmute", l.muted); l.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0)); l.armed = t["armed"]; l.nameColour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("ncol", (juce::int64) 0)); l.nameFont = t["nfont"].toString();
        l.muted = t["mute"]; l.monitor = t.getProperty ("mon", true); l.height = t.getProperty ("h", 56.0f);
        p.lanes.push_back (l);
    }
    for (auto t : root.getChildWithName ("CLIPS"))
    {
        Clip c; c.uid = t["uid"]; c.lane = t["lane"]; c.start = t["start"]; c.length = t["len"]; c.patternIndex = t["pat"]; c.patOffset = t.getProperty ("poff", 0.0);
        c.audioFile = readFile (t, "file"); c.sourceOffset = t["off"]; c.gain = t.getProperty ("gain", 1.0f);
        c.muted = t["mute"]; c.reversed = t["rev"]; c.mixerTrack = t.getProperty ("mix", -1); c.fadeIn = t["fi"]; c.fadeOut = t["fo"];
        c.fadeCurve = t["fc"]; c.takeGroup = t["take"]; c.group = t["grp"]; c.hidden = t["hidden"]; c.drummer = t.getProperty ("drummer", -1);
        c.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0));
        if ((int) t.getProperty ("ak", 0) > 0)
        {
            auto& at = c.autoTarget;
            at.kind = t["ak"]; at.track = t["at"]; at.fxUid = t["afx"]; at.channel = t["ach"]; at.param = t["ap"].toString(); at.name = t["an"].toString();
            for (auto q : t) if (q.hasType ("PT")) c.autoPts.push_back ({ (double) q["b"], (float) q["v"] });
        }
        p.clips.push_back (c);
    }
    for (auto t : root.getChildWithName ("POOL")) p.audioPool.push_back (readFile (t, "file"));
    for (auto t : root.getChildWithName ("MIXER"))
    {
        MixerTrack m; m.name = t["name"].toString(); m.volume = t.getProperty ("vol", 0.8f); m.pan = t["pan"];
        m.muted = t["mute"]; m.solo = t["solo"]; m.frozen = t["frozen"]; m.frozenFile = readFile (t, "frozenFile");
        m.routesToMaster = t.getProperty ("master", true);
        m.duck = t.getProperty ("duck", 0.0f); m.duckSource = t.getProperty ("duckSrc", -1); m.duckThreshold = t.getProperty ("duckTh", -40.0f);
        m.duckRatio = t.getProperty ("duckRatio", 4.0f); m.duckAttack = t.getProperty ("duckAtt", 5.0f); m.duckRelease = t.getProperty ("duckRel", 150.0f);
        m.colour = juce::Colour ((juce::uint32) (juce::int64) t.getProperty ("colour", (juce::int64) 0xff2a2f36));
        {
            juce::StringArray q; q.addTokens (t["meq"].toString(), ",", {});
            if (q.size() == 15)
                for (size_t b = 0; b < 5; ++b)
                { m.meqFreq[b] = q[(int) b * 3].getFloatValue(); m.meqGain[b] = q[(int) b * 3 + 1].getFloatValue(); m.meqQ[b] = q[(int) b * 3 + 2].getFloatValue(); }
        }
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
                migrateFxParams (f.type, f.params);
                m.fx.push_back (f);
            }
        }
        p.mixer.push_back (m);
    }
    if (p.mixer.empty()) { MixerTrack master; master.name = "Master"; master.routesToMaster = false; p.mixer.push_back (master); }
    for (auto t : root.getChildWithName ("CTLLINKS"))
    {
        CtlLink L; L.srcTrack = t["st"]; L.srcUid = t["su"]; L.dstTrack = t["dt"]; L.dstUid = t["du"]; L.param = t["par"].toString();
        L.lo = t.getProperty ("lo", 0.0f); L.hi = t.getProperty ("hi", 1.0f);
        p.ctlLinks.push_back (L);
    }
    // automation of the old AutoPitch "speed" (1 = fastest) -> "retune" (0 = fastest)
    for (auto& c : p.clips)
        if (c.isAuto() && c.autoTarget.kind == AutoFxParam && c.autoTarget.param == "speed"
            && c.autoTarget.track >= 0 && c.autoTarget.track < (int) p.mixer.size())
            for (auto& f : p.mixer[(size_t) c.autoTarget.track].fx)
                if (f.uid == c.autoTarget.fxUid && f.type == "autopitch")
                {
                    c.autoTarget.param = "retune";
                    for (auto& pt : c.autoPts) pt.value = 1.0f - pt.value;
                }
    return p;
}

bool saveProject (const Project& p, const juce::File& f)
{
    IOBaseScope base (f);
    auto xml = projectToTree (p).createXml();
    return xml && xml->writeTo (f);
}

bool loadProject (Project& p, const juce::File& f)
{
    auto xml = juce::XmlDocument::parse (f);
    if (! xml || ! xml->hasTagName ("HUMHOUSE_PROJECT")) return false;
    IOBaseScope base (f);
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

static juce::File freshTempDir()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("hhpack-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
}

bool packTemplate (const Project& src, const juce::String& name, const juce::File& out)
{
    const auto legal = juce::File::createLegalFileName (name.trim().isEmpty() ? juce::String ("Template") : name.trim());
    auto stage = freshTempDir();
    auto root = stage.getChildFile (legal);
    auto sounds = root.getChildFile ("Sounds");
    if (! sounds.createDirectory()) return false;
    Project p = src;
    std::map<juce::String, juce::File> copied;
    auto take = [&] (juce::File& f)
    {
        if (f == juce::File() || ! f.existsAsFile()) return;
        auto it = copied.find (f.getFullPathName());
        if (it == copied.end())
        {
            auto dst = sounds.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), false);
            if (! f.copyFileTo (dst)) return;
            it = copied.emplace (f.getFullPathName(), dst).first;
        }
        f = it->second;
    };
    for (auto& c : p.channels) take (c.sampleFile);
    for (auto& c : p.clips) take (c.audioFile);
    for (auto& m : p.mixer) take (m.frozenFile);
    for (auto& f : p.audioPool) take (f);
    p.projectFile = root.getChildFile (legal + ".hhs");
    bool ok = saveProject (p, p.projectFile);
    if (ok)
    {
        juce::ZipFile::Builder zip;
        for (auto& f : root.findChildFiles (juce::File::findFiles, true, "*"))
            zip.addFile (f, f.hasFileExtension ("hhs") ? 9 : 6, f.getRelativePathFrom (stage).replaceCharacter ('\\', '/'));
        auto tmp = out.getSiblingFile (out.getFileName() + ".tmp");
        {
            juce::FileOutputStream os (tmp);
            ok = os.openedOk() && os.setPosition (0) && os.truncate().wasOk() && zip.writeToStream (os, nullptr);
            os.flush();
        }
        ok = ok && tmp.moveFileTo (out);
        tmp.deleteFile();
    }
    stage.deleteRecursively();
    return ok;
}

juce::File unpackTemplate (const juce::File& pack, const juce::File& templatesDir)
{
    juce::ZipFile zip (pack);
    juce::String top;
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto n = zip.getEntry (i)->filename.replaceCharacter ('\\', '/');
        if (n.startsWithChar ('/') || n.contains ("..") || n.containsChar (':')) return {};   // nothing may land outside the template's folder
        if (top.isEmpty() && n.endsWithIgnoreCase (".hhs") && n.containsChar ('/'))
            top = n.upToFirstOccurrenceOf ("/", false, false);
    }
    if (top.isEmpty()) return {};
    auto stage = freshTempDir();
    if (! stage.createDirectory() || zip.uncompressTo (stage, true).failed()) { stage.deleteRecursively(); return {}; }
    auto from = stage.getChildFile (top);
    auto inner = from.getChildFile (top + ".hhs");
    Project check;
    if (! inner.existsAsFile() || ! loadProject (check, inner)) { stage.deleteRecursively(); return {}; }
    templatesDir.createDirectory();
    auto dest = templatesDir.getNonexistentChildFile (juce::File::createLegalFileName (top), {}, false);
    const bool ok = from.copyDirectoryTo (dest);
    stage.deleteRecursively();
    if (! ok) return {};
    auto hhs = dest.getChildFile (top + ".hhs");
    if (dest.getFileName() != top)
    {
        auto renamed = dest.getChildFile (dest.getFileName() + ".hhs");
        if (hhs.moveFileTo (renamed)) hhs = renamed;
    }
    return hhs.existsAsFile() ? hhs : juce::File();
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
