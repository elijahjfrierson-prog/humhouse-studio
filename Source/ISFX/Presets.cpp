#include "Presets.h"
#include "Editing.h"

namespace isfx
{
juce::File userDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("InSequence FX");
    d.getChildFile ("Presets").createDirectory(); d.getChildFile ("Templates").createDirectory();
    return d;
}

static juce::var fxToVar (const FxStack& f)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : f.p) o->setProperty (k, v);
    auto* r = new juce::DynamicObject();
    r->setProperty ("params", juce::var (o)); r->setProperty ("lut", f.lutPath);
    return juce::var (r);
}

static FxStack fxFromVar (const juce::var& v)
{
    FxStack f;
    if (auto* o = v["params"].getDynamicObject())
        for (auto& pr : o->getProperties()) f.p[pr.name.toString()] = (float) (double) pr.value;
    f.lutPath = v["lut"].toString();
    return f;
}

bool readPresetFile (const juce::File& file, UserPreset& out, juce::String& error)
{
    auto v = juce::JSON::parse (file);
    if (v["format"].toString() != "InSequenceFX-Preset") { error = file.getFileName() + " is not an InSequence FX preset"; return false; }
    out.name = v["name"].toString(); out.clip = v["kind"].toString() == "clip";
    out.fx = fxFromVar (v["fx"]); out.file = file;
    if (out.name.isEmpty()) out.name = file.getFileNameWithoutExtension();
    return true;
}

std::vector<UserPreset> loadUserPresets (const juce::File& dirIn)
{
    auto dir = dirIn == juce::File() ? userDataDir().getChildFile ("Presets") : dirIn;
    std::vector<UserPreset> r;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.isfxpreset"))
    {
        UserPreset p; juce::String e;
        if (readPresetFile (f, p, e)) r.push_back (p);
    }
    std::sort (r.begin(), r.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return r;
}

bool saveUserPreset (const UserPreset& p, juce::File& written, juce::String& error, const juce::File& dirIn)
{
    auto dir = dirIn == juce::File() ? userDataDir().getChildFile ("Presets") : dirIn;
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "InSequenceFX-Preset"); o->setProperty ("version", 1);
    o->setProperty ("name", p.name); o->setProperty ("kind", p.clip ? "clip" : "look");
    o->setProperty ("fx", fxToVar (p.fx));
    written = p.file != juce::File() ? p.file : dir.getChildFile (juce::File::createLegalFileName (p.name) + ".isfxpreset");
    if (! written.replaceWithText (juce::JSON::toString (juce::var (o)))) { error = "Could not write " + written.getFullPathName(); return false; }
    return true;
}

// ------------------------------------------------------------------ templates
bool saveEditTemplate (const Project& p, const Sequence& s, const juce::String& name, const juce::File& out, juce::String& error)
{
    Project t = Project::makeNew();
    t.name = name; t.fps = p.fps; t.aspectW = p.aspectW; t.aspectH = p.aspectH;
    t.song = SongAnalysis(); t.song.valid = true;
    t.song.bpm = p.song.valid ? p.song.bpm : 120.0; t.song.gridOffset = p.song.valid ? p.song.gridOffset : 0;
    t.song.beatsPerBar = p.song.valid ? p.song.beatsPerBar : 4;
    Sequence ts = s; ts.clips.clear();
    std::vector<juce::String> slotOf;
    for (auto& c : s.clips)
    {
        if (c.mediaId == p.songId) continue;                 // the song is re-added from the target project
        auto* m = p.findMedia (c.mediaId);
        if (m == nullptr || m->kind == MediaKind::Audio) continue;
        auto it = std::find (slotOf.begin(), slotOf.end(), c.mediaId);
        int slot = (int) (it - slotOf.begin()); if (it == slotOf.end()) slotOf.push_back (c.mediaId);
        Clip k = c; k.mediaId = "slot:" + juce::String (slot);
        ts.clips.push_back (k);
    }
    if (ts.clips.empty()) { error = "The sequence has no video clips to turn into a template"; return false; }
    ts.name = name;
    t.sequences = { ts }; t.active = 0;
    auto v = toVar (t);
    if (auto* o = v.getDynamicObject()) { o->setProperty ("template", true); o->setProperty ("slots", (int) slotOf.size()); }
    if (! out.replaceWithText (juce::JSON::toString (v))) { error = "Could not write " + out.getFullPathName(); return false; }
    return true;
}

juce::String templateName (const juce::File& f) { auto v = juce::JSON::parse (f); auto n = v["name"].toString(); return n.isEmpty() ? f.getFileNameWithoutExtension() : n; }

std::vector<juce::File> listEditTemplates (const juce::File& dirIn)
{
    auto dir = dirIn == juce::File() ? userDataDir().getChildFile ("Templates") : dirIn;
    auto a = dir.findChildFiles (juce::File::findFiles, false, "*.isfxtemplate");
    a.sort();
    return std::vector<juce::File> (a.begin(), a.end());
}

bool applyEditTemplate (Project& p, const juce::File& f, juce::String& error)
{
    auto v = juce::JSON::parse (f);
    Project t;
    if (! (bool) v["template"] || ! fromVar (v, t, error) || t.sequences.empty()) { if (error.isEmpty()) error = f.getFileName() + " is not an InSequence FX template"; return false; }
    if (! p.song.valid) { error = "Import and analyze a song first: templates are re-locked to the song's beat grid"; return false; }
    std::vector<const MediaItem*> takes;
    for (auto& m : p.media) if (m.id != p.songId && m.kind != MediaKind::Audio && ! m.missing) takes.push_back (&m);
    if (takes.empty()) { error = "Import some video takes first"; return false; }
    std::stable_sort (takes.begin(), takes.end(), [] (auto* a, auto* b)
        { bool sa = a->sync.status == SyncInfo::Synced || a->sync.status == SyncInfo::Manual, sb = b->sync.status == SyncInfo::Synced || b->sync.status == SyncInfo::Manual; return sa && ! sb; });

    const auto& src = t.sequences[0];
    double endBeat = 0;
    for (auto& c : src.clips) endBeat = std::max (endBeat, t.song.beatAt (c.end()));
    const int bpb = std::max (1, p.song.beatsPerBar);
    double pattern = std::max ((double) bpb, std::ceil (endBeat / bpb - 1e-6) * bpb);
    double songBeats = p.song.beatAt (p.song.duration > 0 ? p.song.duration : (p.song_() ? p.song_()->duration : secToTicks (60)));

    Sequence s = src; s.id = newId(); s.clips.clear(); s.name = t.name.isEmpty() ? src.name : t.name; s.style = "template";
    for (int rep = 0; rep * pattern < songBeats; ++rep)
        for (auto& c : src.clips)
        {
            double b0 = t.song.beatAt (c.start) + rep * pattern, b1 = t.song.beatAt (c.end()) + rep * pattern;
            if (b0 >= songBeats) continue;
            b1 = std::min (b1, songBeats);
            Clip k = c; k.id = newId();
            k.start = quantizeToFrame (std::max<Ticks> (0, p.song.gridTime (b0)), p.fps);
            Ticks e = quantizeToFrame (p.song.gridTime (b1), p.fps);
            if (e - k.start < secToTicks (1.0 / p.fps)) continue;
            k.length = e - k.start;
            k.transLen = c.transLen > 0 ? secToTicks (ticksToSec (c.transLen) * t.song.bpm / p.song.bpm) : 0;
            int slot = c.mediaId.fromFirstOccurrenceOf ("slot:", false, false).getIntValue();
            const MediaItem* m = takes[(size_t) slot % takes.size()];
            auto fits = [&] (const MediaItem* mm, Ticks in) { return in >= 0 && in + (Ticks) (k.length * k.speed) <= mm->duration; };
            bool synced = m->sync.status == SyncInfo::Synced || m->sync.status == SyncInfo::Manual;
            Ticks in = synced ? k.start - m->sync.offset : c.srcIn;
            if (! fits (m, in) || m->kind == MediaKind::Image)
            {
                for (auto* o : takesCovering (p, k.start, k.length)) { m = o; in = k.start - o->sync.offset; break; }
                if (! fits (m, in)) in = juce::jlimit<Ticks> (0, std::max<Ticks> (0, m->duration - (Ticks) (k.length * k.speed)), c.srcIn);
            }
            if (m->kind == MediaKind::Image) in = 0;
            k.mediaId = m->id; k.srcIn = in; k.volume = 0;
            s.clips.push_back (k);
        }
    int at = -1;
    for (int i = 0; i < (int) s.tracks.size(); ++i) if (s.tracks[(size_t) i].kind == TrackKind::Audio) { at = i; break; }
    if (at < 0) at = s.addTrack (TrackKind::Audio, "SONG");
    if (auto* sm = p.song_())
    {
        Clip sc; sc.id = newId(); sc.mediaId = sm->id; sc.track = at; sc.start = 0; sc.length = sm->duration; sc.volume = 1; sc.locked = true;
        s.clips.push_back (sc);
    }
    s.sortClips();
    p.sequences.push_back (s);
    p.active = (int) p.sequences.size() - 1;
    p.aspectW = t.aspectW; p.aspectH = t.aspectH;
    return true;
}

bool renameEditTemplate (const juce::File& f, const juce::String& name, juce::String& error)
{
    auto v = juce::JSON::parse (f);
    auto* o = v.getDynamicObject();
    if (o == nullptr || ! (bool) v["template"]) { error = f.getFileName() + " is not an edit template"; return false; }
    o->setProperty ("name", name);
    if (auto* seqs = v["sequences"].getArray())
        for (auto& sq : *seqs) if (auto* so = sq.getDynamicObject()) so->setProperty ("name", name);
    if (! f.replaceWithText (juce::JSON::toString (v))) { error = "Could not write " + f.getFullPathName(); return false; }
    return true;
}

static juce::File recentStore (const juce::File& store) { return store == juce::File() ? userDataDir().getChildFile ("recent.txt") : store; }

juce::Array<juce::File> recentProjects (const juce::File& store)
{
    juce::StringArray lines; lines.addLines (recentStore (store).loadFileAsString());
    juce::Array<juce::File> out;
    for (auto& l : lines)
        if (auto path = l.trim(); juce::File::isAbsolutePath (path))
            if (juce::File f (path); f.existsAsFile() && ! out.contains (f)) out.add (f);
    return out;
}

void noteRecentProject (const juce::File& f, const juce::File& store)
{
    auto a = recentProjects (store);
    a.removeAllInstancesOf (f); a.insert (0, f);
    juce::StringArray lines;
    for (int i = 0; i < juce::jmin (12, a.size()); ++i) lines.add (a[i].getFullPathName());
    auto out = recentStore (store);
    out.getParentDirectory().createDirectory();
    out.replaceWithText (lines.joinIntoString ("\n"));
}
}
