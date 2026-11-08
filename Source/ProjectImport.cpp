#include "ProjectImport.h"

namespace hhs
{
namespace
{
float faderFromGain (double g) { return (float) juce::jlimit (0.0, 1.0, 0.8 * std::sqrt (juce::jmax (0.0, g))); }

double fileSeconds (const juce::File& f)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    return r != nullptr && r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
}

juce::File resolve (const juce::String& path, const juce::File& base)
{
    if (path.isEmpty()) return {};
    const auto p = path.replaceCharacter ('\\', '/');
    if (juce::File::isAbsolutePath (p) && juce::File (p).existsAsFile()) return juce::File (p);
    for (auto d = base; d.exists() && d != d.getParentDirectory(); d = d.getParentDirectory())
    {
        if (auto c = d.getChildFile (p); c.existsAsFile()) return c;
        for (auto* sub : { "", "Audio Files", "Media", "Samples", "Samples/Recorded", "Samples/Imported" })
            if (auto c = d.getChildFile (sub).getChildFile (juce::File (p).getFileName()); c.existsAsFile()) return c;
        if (d != base) break;   // look in the project folder and one level up
    }
    return juce::File::isAbsolutePath (p) ? juce::File (p) : base.getChildFile (p);
}

Project blank()
{
    Project p;
    p.songMode = true;   // converted songs play their arrangement, not an empty pattern
    MixerTrack master; master.name = "Master"; master.routesToMaster = false;
    p.mixer.push_back (master);
    Pattern pat; pat.name = "Pattern 1";
    p.patterns.push_back (pat);
    return p;
}

int addTrack (Project& p, const juce::String& name, float vol, float pan)
{
    Lane l; l.name = name.isNotEmpty() ? name : "Track " + juce::String ((int) p.lanes.size() + 1);
    l.mixerTrack = p.addMixerTrack (l.name);
    auto& mt = p.mixer[(size_t) l.mixerTrack];
    mt.volume = vol; mt.pan = juce::jlimit (-1.0f, 1.0f, pan);
    p.lanes.push_back (l);
    return (int) p.lanes.size() - 1;
}

void addAudio (Project& p, int lane, const juce::File& f, double startBeats, double lenBeats, double offsetSecs, juce::StringArray& missing)
{
    if (! f.existsAsFile()) { missing.addIfNotAlreadyThere (f.getFileName()); return; }
    Clip c; c.uid = p.newUid(); c.lane = lane; c.audioFile = f; c.start = juce::jmax (0.0, startBeats);
    c.sourceOffset = juce::jmax (0.0, offsetSecs);
    c.length = lenBeats > 0 ? lenBeats : secondsToBeats (fileSeconds (f) - c.sourceOffset, p.bpm);
    if (c.length <= 0) return;
    p.clips.push_back (c);
    p.audioPool.push_back (f);
}

void addFxByName (Project& p, int lane, const juce::String& name, const PluginFinder& find, juce::StringArray& fxOk, juce::StringArray& fxMissing)
{
    if (name.isEmpty()) return;
    const auto id = find ? find (name, false) : juce::String();
    if (id.isEmpty()) { fxMissing.addIfNotAlreadyThere (name); return; }
    auto& s = p.addFx (p.lanes[(size_t) lane].mixerTrack, "plugin");
    s.pluginId = id;
    fxOk.addIfNotAlreadyThere (name);
}

// "VST3: AmpliTube 5 (IK Multimedia)" -> "AmpliTube 5"
juce::String cleanPluginName (juce::String s)
{
    if (s.containsChar (':')) s = s.fromFirstOccurrenceOf (":", false, false);
    s = s.trim();
    if (s.endsWithChar (')') && s.containsChar ('(')) s = s.upToLastOccurrenceOf ("(", false, false).trim();
    return s;
}

void finish (Project& p, juce::StringArray& report, const juce::StringArray& missing, const juce::StringArray& fxOk, const juce::StringArray& fxMissing, int clips)
{
    if (p.lanes.empty()) addTrack (p, "Track 1", 0.8f, 0.0f);
    if (p.clips.empty()) p.songMode = false;   // patterns only (no playlist): play the pattern
    report.add (juce::String ((int) p.lanes.size()) + " tracks, " + juce::String (clips) + " clips, " + juce::String (p.bpm, 2) + " BPM");
    if (! fxOk.isEmpty()) report.add ("FX loaded: " + fxOk.joinIntoString (", ") + " (settings start at the plugin's default)");
    if (! fxMissing.isEmpty()) report.add ("FX not installed here: " + fxMissing.joinIntoString (", "));
    if (! missing.isEmpty()) report.add ("Audio not found: " + missing.joinIntoString (", "));
}

// ---------------- Reaper .rpp (plain text) ----------------
juce::StringArray rppTokens (const juce::String& line)
{
    juce::StringArray t;
    auto s = line.trim();
    int i = 0;
    while (i < s.length())
    {
        while (i < s.length() && juce::CharacterFunctions::isWhitespace (s[i])) ++i;
        if (i >= s.length()) break;
        const auto q = s[i];
        if (q == '"' || q == '\'' || q == '`')
        {
            const int e = s.indexOfChar (i + 1, q);
            t.add (s.substring (i + 1, e < 0 ? s.length() : e));
            i = e < 0 ? s.length() : e + 1;
        }
        else
        {
            int e = i; while (e < s.length() && ! juce::CharacterFunctions::isWhitespace (s[e])) ++e;
            t.add (s.substring (i, e)); i = e;
        }
    }
    return t;
}

bool importRpp (const juce::File& f, Project& p, juce::StringArray& report, const PluginFinder& find)
{
    const auto lines = juce::StringArray::fromLines (f.loadFileAsString());
    if (lines.isEmpty() || ! lines[0].trim().startsWith ("<REAPER_PROJECT")) return false;
    struct Item { int lane; double pos, len, offs; juce::String file; };
    std::vector<Item> items;
    juce::StringArray stack, missing, fxOk, fxMissing;
    int lane = -1; Item cur {};
    for (auto& raw : lines)
    {
        const auto tk = rppTokens (raw);
        if (tk.isEmpty()) continue;
        if (tk[0] == ">")
        {
            if (stack.isEmpty()) continue;
            const auto top = stack[stack.size() - 1];
            if (top == "ITEM" && lane >= 0) items.push_back (cur);
            stack.remove (stack.size() - 1);
            continue;
        }
        if (tk[0].startsWithChar ('<'))
        {
            const auto tag = tk[0].substring (1);
            if (tag == "TRACK") lane = addTrack (p, {}, 0.8f, 0.0f);
            if (tag == "ITEM") cur = { lane, 0, 0, 0, {} };
            if ((tag == "VST" || tag == "AU" || tag == "CLAP") && stack.contains ("FXCHAIN") && stack.contains ("TRACK") && lane >= 0)
            {
                addFxByName (p, lane, cleanPluginName (tk[1]), find, fxOk, fxMissing);
            }
            if (tag == "JS" && stack.contains ("TRACK")) fxMissing.addIfNotAlreadyThere ("JS: " + tk[1]);
            stack.add (tag);
            continue;
        }
        const auto top = stack.isEmpty() ? juce::String() : stack[stack.size() - 1];
        if (top == "REAPER_PROJECT" && tk[0] == "TEMPO" && tk.size() > 1) p.bpm = juce::jlimit (20.0, 999.0, tk[1].getDoubleValue());
        else if (top == "TRACK" && lane >= 0)
        {
            auto& mt = p.mixer[(size_t) p.lanes[(size_t) lane].mixerTrack];
            if (tk[0] == "NAME" && tk[1].isNotEmpty()) p.lanes[(size_t) lane].name = mt.name = tk[1];
            else if (tk[0] == "VOLPAN" && tk.size() > 2) { mt.volume = faderFromGain (tk[1].getDoubleValue()); mt.pan = juce::jlimit (-1.0f, 1.0f, (float) tk[2].getDoubleValue()); }
        }
        else if (top == "ITEM")
        {
            if (tk[0] == "POSITION") cur.pos = tk[1].getDoubleValue();
            else if (tk[0] == "LENGTH") cur.len = tk[1].getDoubleValue();
            else if (tk[0] == "SOFFS") cur.offs = tk[1].getDoubleValue();
        }
        else if (top == "SOURCE" && tk[0] == "FILE" && stack.contains ("ITEM") && cur.file.isEmpty()) cur.file = tk[1];
    }
    for (auto& it : items)
        if (it.file.isNotEmpty() && it.lane >= 0 && it.lane < (int) p.lanes.size())
            addAudio (p, it.lane, resolve (it.file, f.getParentDirectory()), secondsToBeats (it.pos, p.bpm), secondsToBeats (it.len, p.bpm), it.offs, missing);
    finish (p, report, missing, fxOk, fxMissing, (int) p.clips.size());
    return true;
}

// ---------------- Ableton .als (gzipped XML) ----------------
juce::String val (const juce::XmlElement* e, const char* child)
{
    if (e == nullptr) return {};
    if (auto* c = e->getChildByName (child)) return c->getStringAttribute ("Value");
    return {};
}
const juce::XmlElement* path (const juce::XmlElement* e, std::initializer_list<const char*> names)
{
    for (auto* n : names) { if (e == nullptr) return nullptr; e = e->getChildByName (n); }
    return e;
}
template <typename Fn> void forEachDeep (const juce::XmlElement* e, const char* tag, Fn&& fn)
{
    if (e == nullptr) return;
    for (auto* c : e->getChildIterator()) { if (c->hasTagName (tag)) fn (c); else forEachDeep (c, tag, fn); }
}

bool importAls (const juce::File& f, Project& p, juce::StringArray& report, const PluginFinder& find)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return false;
    juce::GZIPDecompressorInputStream gz (&in, false, juce::GZIPDecompressorInputStream::gzipFormat);
    const auto text = gz.readEntireStreamAsString();
    auto xml = juce::XmlDocument::parse (text.isNotEmpty() ? text : f.loadFileAsString());
    if (xml == nullptr || ! xml->hasTagName ("Ableton")) return false;
    auto* set = xml->getChildByName ("LiveSet");
    if (set == nullptr) return false;
    juce::StringArray missing, fxOk, fxMissing;
    for (auto* mtag : { "MasterTrack", "MainTrack" })
        if (auto* t = path (set, { mtag, "DeviceChain", "Mixer", "Tempo" })) p.bpm = juce::jlimit (20.0, 999.0, val (t, "Manual").getDoubleValue());
    int clips = 0;
    auto* tracks = set->getChildByName ("Tracks");
    if (tracks == nullptr) return false;
    for (auto* t : tracks->getChildIterator())
    {
        const bool audio = t->hasTagName ("AudioTrack"), midi = t->hasTagName ("MidiTrack");
        if (! audio && ! midi) continue;
        const auto name = val (t->getChildByName ("Name"), "EffectiveName");
        auto* mix = path (t, { "DeviceChain", "Mixer" });
        const int lane = addTrack (p, name, faderFromGain (val (path (mix, { "Volume" }), "Manual").getDoubleValue() > 0 ? val (path (mix, { "Volume" }), "Manual").getDoubleValue() : 1.0),
                                   (float) val (path (mix, { "Pan" }), "Manual").getDoubleValue());
        int chId = -1;
        // devices: an instrument on a MIDI track becomes the channel, the rest become insert FX
        bool first = true;
        forEachDeep (path (t, { "DeviceChain" }), "PluginDesc", [&] (const juce::XmlElement* d)
        {
            juce::String nm;
            for (auto* info : { "VstPluginInfo", "Vst3PluginInfo", "AuPluginInfo" })
                if (auto* i = d->getChildByName (info)) { nm = val (i, "PlugName"); if (nm.isEmpty()) nm = val (i, "Name"); }
            if (nm.isEmpty()) return;
            if (midi && first)
            {
                first = false;
                if (auto id = find ? find (nm, true) : juce::String(); id.isNotEmpty())
                {
                    Channel c; c.name = nm; c.type = ChannelType::Plugin; c.pluginId = id; c.mixerTrack = p.lanes[(size_t) lane].mixerTrack;
                    chId = p.addChannel (c); fxOk.addIfNotAlreadyThere (nm); return;
                }
                fxMissing.addIfNotAlreadyThere (nm); return;
            }
            addFxByName (p, lane, nm, find, fxOk, fxMissing);
        });
        if (midi && chId < 0)
        {
            Channel c; c.name = name.isNotEmpty() ? name : "MIDI"; c.type = ChannelType::Synth; c.mixerTrack = p.lanes[(size_t) lane].mixerTrack;
            chId = p.addChannel (c);
        }
        forEachDeep (t, audio ? "AudioClip" : "MidiClip", [&] (const juce::XmlElement* c)
        {
            if (val (c, "Disabled") == "true") return;
            const double s = val (c, "CurrentStart").getDoubleValue(), e = val (c, "CurrentEnd").getDoubleValue();
            if (e <= s) return;
            if (audio)
            {
                auto* ref = path (c, { "SampleRef", "FileRef" });
                juce::String fp = val (ref, "Path");
                if (fp.isEmpty()) fp = val (ref, "RelativePath");
                if (fp.isEmpty() && ref != nullptr)
                    if (auto* rp = ref->getChildByName ("RelativePath"))
                        for (auto* el : rp->getChildIterator()) fp += (fp.isEmpty() ? "" : "/") + el->getStringAttribute ("Dir");
                if (fp.isEmpty()) fp = val (ref, "Name");
                const double loopStart = val (path (c, { "Loop" }), "LoopStart").getDoubleValue();
                addAudio (p, lane, resolve (fp, f.getParentDirectory()), s, e - s, beatsToSeconds (loopStart, p.bpm), missing);
                ++clips;
                return;
            }
            Pattern pat; pat.name = val (c, "Name").isNotEmpty() ? val (c, "Name") : name;
            const double loopStart = val (path (c, { "Loop" }), "LoopStart").getDoubleValue();
            pat.loopBeats = juce::jmax (0.0, val (path (c, { "Loop" }), "LoopEnd").getDoubleValue() - loopStart);   // repeat at the Ableton loop, not every bar
            auto& notes = pat.notes[chId];
            forEachDeep (path (c, { "Notes" }), "KeyTrack", [&] (const juce::XmlElement* k)
            {
                const int key = val (k, "MidiKey").getIntValue();
                forEachDeep (k, "MidiNoteEvent", [&] (const juce::XmlElement* ev)
                {
                    if (ev->getStringAttribute ("IsEnabled", "true") == "false") return;
                    Note n; n.key = juce::jlimit (0, 127, key);
                    n.start = ev->getDoubleAttribute ("Time") - loopStart; n.length = juce::jmax (0.01, ev->getDoubleAttribute ("Duration"));
                    n.velocity = (float) juce::jlimit (0.0, 1.0, ev->getDoubleAttribute ("Velocity", 100) / 127.0);
                    if (n.start >= 0) notes.push_back (n);
                });
            });
            p.patterns.push_back (pat);
            Clip cl; cl.uid = p.newUid(); cl.lane = lane; cl.start = s; cl.length = e - s; cl.patternIndex = (int) p.patterns.size() - 1;
            p.clips.push_back (cl);
            ++clips;
        });
    }
    finish (p, report, missing, fxOk, fxMissing, clips);
    return true;
}

// ---------------- FL Studio .flp (binary events) ----------------
juce::String flText (const juce::MemoryBlock& d, bool utf16)
{
    if (d.getSize() == 0) return {};
    if (utf16 && d.getSize() >= 2)
        return juce::String (juce::CharPointer_UTF16 ((const juce::CharPointer_UTF16::CharType*) d.getData()), d.getSize() / 2).trimCharactersAtEnd (juce::String::charToString (0));
    return juce::String::fromUTF8 ((const char*) d.getData(), (int) d.getSize()).trimCharactersAtEnd (juce::String::charToString (0));
}

bool importFlp (const juce::File& f, Project& p, juce::StringArray& report, const PluginFinder& find)
{
    juce::MemoryBlock all;
    if (! f.loadFileAsData (all) || all.getSize() < 22 || std::memcmp (all.getData(), "FLhd", 4) != 0) return false;
    const auto* b = (const juce::uint8*) all.getData();
    auto u16 = [&] (size_t o) { return (int) (b[o] | (b[o + 1] << 8)); };
    auto u32 = [&] (size_t o) { return (juce::uint32) b[o] | ((juce::uint32) b[o + 1] << 8) | ((juce::uint32) b[o + 2] << 16) | ((juce::uint32) b[o + 3] << 24); };
    const int ppq = juce::jmax (1, u16 (12));
    size_t pos = 14;
    if (std::memcmp (b + pos, "FLdt", 4) != 0) return false;
    pos += 8;
    const size_t end = all.getSize();
    bool utf16 = false; int major = 0;
    struct Chan { juce::String name, sample, plugin; };
    std::vector<Chan> chans; int curChan = -1;
    std::map<int, std::vector<std::array<juce::uint32, 4>>> patNotes;   // pattern -> pos, chan, len, key|vel<<8
    std::map<int, juce::String> patNames; int curPat = 0;
    struct Item { juce::uint32 at, len; int pattern, chan, track; };
    std::vector<Item> items;
    while (pos < end)
    {
        const int id = b[pos++];
        juce::uint32 v = 0; juce::MemoryBlock data;
        if (id < 64) { if (pos + 1 > end) break; v = b[pos]; pos += 1; }
        else if (id < 128) { if (pos + 2 > end) break; v = (juce::uint32) u16 (pos); pos += 2; }
        else if (id < 192) { if (pos + 4 > end) break; v = u32 (pos); pos += 4; }
        else
        {
            size_t len = 0; int sh = 0;
            while (pos < end) { const auto c = b[pos++]; len |= (size_t) (c & 0x7f) << sh; sh += 7; if (! (c & 0x80) || sh > 28) break; }
            if (pos + len > end) break;
            data.append (b + pos, len); pos += len;
        }
        switch (id)
        {
            case 199: { const auto ver = juce::String::fromUTF8 ((const char*) data.getData(), (int) data.getSize()); major = ver.upToFirstOccurrenceOf (".", false, false).getIntValue(); utf16 = major >= 12 || (major == 11 && ver.contains ("11.5")); break; }
            case 66: if (v > 0) p.bpm = juce::jlimit (20.0, 999.0, (double) v); break;
            case 156: if (v > 0) p.bpm = juce::jlimit (20.0, 999.0, v / 1000.0); break;
            case 64: curChan = (int) v; if ((int) chans.size() <= curChan) chans.resize ((size_t) curChan + 1); break;
            case 65: curPat = (int) v; break;
            case 192: if (curChan >= 0) chans[(size_t) curChan].name = flText (data, utf16); break;
            case 203: if (curChan >= 0 && chans[(size_t) curChan].name.isEmpty()) chans[(size_t) curChan].name = flText (data, utf16); break;
            case 201: if (curChan >= 0) chans[(size_t) curChan].plugin = flText (data, utf16); break;
            case 196: if (curChan >= 0) chans[(size_t) curChan].sample = flText (data, utf16); break;
            case 193: patNames[curPat] = flText (data, utf16); break;
            case 224:
                for (size_t o = 0; o + 24 <= data.getSize(); o += 24)
                {
                    const auto* n = (const juce::uint8*) data.getData() + o;
                    auto r32 = [n] (int k) { return (juce::uint32) n[k] | ((juce::uint32) n[k + 1] << 8) | ((juce::uint32) n[k + 2] << 16) | ((juce::uint32) n[k + 3] << 24); };
                    patNotes[curPat].push_back ({ r32 (0), (juce::uint32) (n[6] | (n[7] << 8)), r32 (8), (juce::uint32) n[12] | ((juce::uint32) n[21] << 8) });
                }
                break;
            case 233:
            {
                const size_t sz = major >= 21 && data.getSize() % 60 == 0 ? 60 : 32;
                for (size_t o = 0; o + sz <= data.getSize(); o += sz)
                {
                    const auto* n = (const juce::uint8*) data.getData() + o;
                    auto r16 = [n] (int k) { return (int) (n[k] | (n[k + 1] << 8)); };
                    auto r32 = [n] (int k) { return (juce::uint32) n[k] | ((juce::uint32) n[k + 1] << 8) | ((juce::uint32) n[k + 2] << 16) | ((juce::uint32) n[k + 3] << 24); };
                    const int base = r16 (4), idx = r16 (6);
                    items.push_back ({ r32 (0), r32 (8), idx > base ? idx - base : -1, idx > base ? -1 : idx, 499 - r16 (12) });
                }
                break;
            }
            default: break;
        }
    }
    juce::StringArray missing, fxOk, fxMissing;
    std::map<int, int> chanId;   // FL channel index -> ours
    for (size_t i = 0; i < chans.size(); ++i)
    {
        auto& c = chans[i];
        Channel ch; ch.name = c.name.isNotEmpty() ? c.name : (c.sample.isNotEmpty() ? juce::File (c.sample.replaceCharacter ('\\', '/')).getFileNameWithoutExtension() : "Channel " + juce::String ((int) i + 1));
        ch.mixerTrack = -1;
        if (c.sample.isNotEmpty())
        {
            ch.sampleFile = resolve (c.sample, f.getParentDirectory());
            if (! ch.sampleFile.existsAsFile()) missing.addIfNotAlreadyThere (ch.sampleFile.getFileName());
            ch.keyTrack = false;
        }
        else if (c.plugin.isNotEmpty() && c.plugin != "Sampler" && c.plugin != "Audio Clip")
        {
            const auto nm = c.name.isNotEmpty() ? c.name : c.plugin;
            if (auto id = find ? find (nm, true) : juce::String(); id.isNotEmpty()) { ch.type = ChannelType::Plugin; ch.pluginId = id; fxOk.addIfNotAlreadyThere (nm); }
            else { ch.type = ChannelType::Synth; fxMissing.addIfNotAlreadyThere (nm); }
        }
        else if (c.plugin == "Audio Clip") continue;
        chanId[(int) i] = p.addChannel (ch);
    }
    p.patterns.clear();
    std::map<int, int> patIndex;
    for (auto& [k, notes] : patNotes)
    {
        Pattern pat; pat.name = patNames.count (k) ? patNames[k] : "Pattern " + juce::String (k);
        for (auto& n : notes)
        {
            auto it = chanId.find ((int) n[1]);
            if (it == chanId.end()) continue;
            Note x; x.start = (double) n[0] / ppq; x.length = juce::jmax (1.0 / 16, (double) n[2] / ppq);
            x.key = juce::jlimit (0, 127, (int) (n[3] & 0xff)); x.velocity = juce::jlimit (0.0f, 1.0f, (float) ((n[3] >> 8) & 0xff) / 128.0f);
            pat.notes[it->second].push_back (x);
        }
        patIndex[k] = (int) p.patterns.size();
        p.patterns.push_back (pat);
    }
    if (p.patterns.empty()) { Pattern pat; pat.name = "Pattern 1"; p.patterns.push_back (pat); }
    int maxTrack = -1;
    for (auto& it : items) maxTrack = juce::jmax (maxTrack, juce::jlimit (0, 498, it.track));
    for (int t = 0; t <= maxTrack; ++t) addTrack (p, "Track " + juce::String (t + 1), 0.8f, 0.0f);
    int clips = 0;
    for (auto& it : items)
    {
        const int lane = juce::jlimit (0, juce::jmax (0, maxTrack), it.track);
        if (lane >= (int) p.lanes.size()) continue;
        if (it.pattern > 0 && patIndex.count (it.pattern))
        {
            Clip c; c.uid = p.newUid(); c.lane = lane; c.start = (double) it.at / ppq; c.length = juce::jmax (0.25, (double) it.len / ppq);
            c.patternIndex = patIndex[it.pattern]; p.clips.push_back (c); ++clips;
        }
        else if (it.chan >= 0 && it.chan < (int) chans.size() && chans[(size_t) it.chan].sample.isNotEmpty())
        {
            addAudio (p, lane, resolve (chans[(size_t) it.chan].sample, f.getParentDirectory()), (double) it.at / ppq, (double) it.len / ppq, 0.0, missing);
            ++clips;
        }
    }
    report.add ("FL Studio: channels, samples, patterns and playlist read. Mixer FX chains and plugin settings live in FL's own format and aren't copied.");
    finish (p, report, missing, fxOk, fxMissing, clips);
    return true;
}
}

bool isForeignProject (const juce::File& f) { return f.hasFileExtension ("rpp;als;flp"); }

bool importForeignProject (const juce::File& f, Project& out, juce::StringArray& report, const PluginFinder& find)
{
    Project p = blank();
    p.title = f.getFileNameWithoutExtension();
    bool ok = false;
    if (f.hasFileExtension ("rpp")) ok = importRpp (f, p, report, find);
    else if (f.hasFileExtension ("als")) ok = importAls (f, p, report, find);
    else if (f.hasFileExtension ("flp")) ok = importFlp (f, p, report, find);
    if (! ok) return false;
    out = std::move (p);
    return true;
}

bool importStemFolder (const juce::File& dir, Project& out, juce::StringArray& report)
{
    if (! dir.isDirectory()) return false;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    if (files.isEmpty()) return false;
    files.sort();
    Project p = blank();
    p.title = dir.getFileName();
    // a tempo in the folder or file names ("Song 92bpm") sets the grid; otherwise keep the default
    for (auto& s : juce::StringArray { dir.getFileName(), files[0].getFileNameWithoutExtension() })
    {
        const auto low = s.toLowerCase();
        if (const int k = low.indexOf ("bpm"); k > 0)
        {
            int a = k; while (a > 0 && (juce::CharacterFunctions::isDigit (low[a - 1]) || low[a - 1] == '.' || low[a - 1] == ' ')) --a;
            if (const double v = low.substring (a, k).trim().getDoubleValue(); v >= 40 && v <= 300) { p.bpm = v; break; }
        }
    }
    juce::StringArray missing;
    for (auto& f : files)
    {
        const int lane = addTrack (p, f.getFileNameWithoutExtension(), 0.8f, 0.0f);
        addAudio (p, lane, f, 0.0, 0.0, 0.0, missing);
    }
    finish (p, report, missing, {}, {}, (int) p.clips.size());
    out = std::move (p);
    return true;
}
}
