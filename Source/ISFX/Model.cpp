#include "Model.h"
#include <algorithm>

namespace isfx
{
juce::String newId() { return juce::Uuid().toString().substring (0, 12); }

int SongAnalysis::sectionAt (Ticks t) const
{
    if (barSection.empty()) return 1;
    return barSection[(size_t) juce::jlimit (0, (int) barSection.size() - 1, barAt (t))];
}
float SongAnalysis::energyAt (Ticks t) const
{
    if (barEnergy.empty()) return 0.5f;
    return barEnergy[(size_t) juce::jlimit (0, (int) barEnergy.size() - 1, barAt (t))];
}

Ticks Sequence::duration() const
{
    Ticks d = 0;
    for (auto& c : clips) d = std::max (d, c.end());
    return d;
}
Clip* Sequence::findClip (const juce::String& cid)
{
    for (auto& c : clips) if (c.id == cid) return &c;
    return nullptr;
}
const Clip* Sequence::findClip (const juce::String& cid) const
{
    for (auto& c : clips) if (c.id == cid) return &c;
    return nullptr;
}
int Sequence::videoTrackCount() const
{
    int n = 0; for (auto& t : tracks) if (t.kind == TrackKind::Video) ++n; return n;
}
int Sequence::addTrack (TrackKind k, const juce::String& nm)
{
    Track t; t.kind = k; t.name = nm; t.height = k == TrackKind::Audio ? 56 : 64;
    if (k == TrackKind::Video)
    {
        // video tracks sit above audio tracks
        int insertAt = 0;
        for (int i = 0; i < (int) tracks.size(); ++i) if (tracks[(size_t) i].kind == TrackKind::Video) insertAt = i + 1;
        tracks.insert (tracks.begin() + insertAt, t);
        for (auto& c : clips) if (c.track >= insertAt) ++c.track;
        return insertAt;
    }
    tracks.push_back (t);
    return (int) tracks.size() - 1;
}
void Sequence::sortClips()
{
    std::stable_sort (clips.begin(), clips.end(), [] (const Clip& a, const Clip& b)
                      { return a.track != b.track ? a.track < b.track : a.start < b.start; });
}

MediaItem* Project::findMedia (const juce::String& mid)
{
    for (auto& m : media) if (m.id == mid) return &m;
    return nullptr;
}
const MediaItem* Project::findMedia (const juce::String& mid) const
{
    for (auto& m : media) if (m.id == mid) return &m;
    return nullptr;
}
Sequence Project::makeDefaultSequence (const juce::String& nm)
{
    Sequence s; s.id = newId(); s.name = nm;
    s.addTrack (TrackKind::Video, "V1");
    s.addTrack (TrackKind::Video, "V2");
    s.addTrack (TrackKind::Audio, "SONG");
    s.tracks.back().locked = false;
    return s;
}
Project Project::makeNew()
{
    Project p; p.sequences.push_back (makeDefaultSequence ("Main Music Video"));
    return p;
}

// ---------------------------------------------------------------- JSON
static juce::String t2s (Ticks t) { return juce::String (t); }
static Ticks s2t (const juce::var& v) { return v.toString().getLargeIntValue(); }

static juce::var fxToVar (const FxStack& f)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : f.p) o->setProperty (k, v);
    juce::var r (new juce::DynamicObject());
    r.getDynamicObject()->setProperty ("p", juce::var (o));
    if (f.lutPath.isNotEmpty()) r.getDynamicObject()->setProperty ("lut", f.lutPath);
    return r;
}
static void fxFromVar (const juce::var& v, FxStack& f)
{
    f.p.clear();
    if (auto* o = v["p"].getDynamicObject())
        for (auto& nv : o->getProperties()) f.p[nv.name.toString()] = (float) (double) nv.value;
    f.lutPath = v["lut"].toString();
}

juce::var toVar (const Project& p)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "InSequenceFX");
    root->setProperty ("version", Project::kVersion);
    root->setProperty ("name", p.name);
    root->setProperty ("active", p.active);
    root->setProperty ("songId", p.songId);
    root->setProperty ("plugins", p.plugins);
    root->setProperty ("fps", p.fps);
    root->setProperty ("aspectW", p.aspectW);
    root->setProperty ("aspectH", p.aspectH);
    root->setProperty ("musicLock", p.musicLock);

    juce::Array<juce::var> media;
    for (auto& m : p.media)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", m.id); o->setProperty ("path", m.path); o->setProperty ("name", m.name);
        o->setProperty ("kind", (int) m.kind); o->setProperty ("duration", t2s (m.duration));
        o->setProperty ("fps", m.fps); o->setProperty ("w", m.width); o->setProperty ("h", m.height);
        o->setProperty ("rot", m.rotation); o->setProperty ("proxy", m.proxyPath); o->setProperty ("audio", m.hasAudio); o->setProperty ("fav", m.favorite);
        o->setProperty ("quality", m.quality);
        o->setProperty ("syncStatus", (int) m.sync.status); o->setProperty ("syncOffset", t2s (m.sync.offset));
        o->setProperty ("syncConf", m.sync.confidence); o->setProperty ("syncNote", m.sync.note);
        media.add (juce::var (o));
    }
    root->setProperty ("media", media);

    auto* a = new juce::DynamicObject();
    a->setProperty ("valid", p.song.valid); a->setProperty ("bpm", p.song.bpm);
    a->setProperty ("offset", t2s (p.song.gridOffset)); a->setProperty ("bpb", p.song.beatsPerBar);
    a->setProperty ("duration", t2s (p.song.duration));
    juce::Array<juce::var> ev;
    for (auto& e : p.song.events)
    {
        juce::Array<juce::var> one { t2s (e.time), (int) e.type, e.strength, e.confidence };
        ev.add (one);
    }
    a->setProperty ("events", ev);
    juce::Array<juce::var> be, bs;
    for (auto x : p.song.barEnergy) be.add (x);
    for (auto x : p.song.barSection) bs.add (x);
    a->setProperty ("barEnergy", be); a->setProperty ("barSection", bs);
    root->setProperty ("analysis", juce::var (a));

    juce::Array<juce::var> seqs;
    for (auto& s : p.sequences)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", s.id); o->setProperty ("name", s.name); o->setProperty ("style", s.style);
        o->setProperty ("seed", (int) s.seed); o->setProperty ("fx", fxToVar (s.fx));
        juce::Array<juce::var> tr;
        for (auto& t : s.tracks)
        {
            auto* to = new juce::DynamicObject();
            to->setProperty ("name", t.name); to->setProperty ("kind", (int) t.kind); to->setProperty ("mute", t.mute);
            to->setProperty ("hidden", t.hidden); to->setProperty ("solo", t.solo); to->setProperty ("locked", t.locked); to->setProperty ("height", t.height);
            if (t.id.isNotEmpty()) to->setProperty ("id", t.id);
            if (t.plugins.isArray()) to->setProperty ("plugins", t.plugins);
            tr.add (juce::var (to));
        }
        o->setProperty ("tracks", tr);
        juce::Array<juce::var> cl;
        for (auto& c : s.clips)
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("id", c.id); co->setProperty ("media", c.mediaId); co->setProperty ("track", c.track);
            co->setProperty ("start", t2s (c.start)); co->setProperty ("length", t2s (c.length)); co->setProperty ("srcIn", t2s (c.srcIn));
            co->setProperty ("speed", c.speed); co->setProperty ("locked", c.locked); co->setProperty ("group", c.group);
            co->setProperty ("opacity", c.opacity); co->setProperty ("volume", c.volume);
            co->setProperty ("trans", (int) c.transIn); co->setProperty ("transLen", t2s (c.transLen));
            co->setProperty ("fx", fxToVar (c.fx));
            if (c.name.isNotEmpty()) co->setProperty ("name", c.name);
            if (! c.enabled) co->setProperty ("enabled", false);
            if (c.reverse) co->setProperty ("reverse", true);
            if (c.label != 0) co->setProperty ("label", c.label);
            if (c.keep) co->setProperty ("keep", true);
            cl.add (juce::var (co));
        }
        o->setProperty ("clips", cl);
        seqs.add (juce::var (o));
    }
    root->setProperty ("sequences", seqs);

    auto* ex = new juce::DynamicObject();
    ex->setProperty ("w", p.exportSettings.width); ex->setProperty ("h", p.exportSettings.height);
    ex->setProperty ("fps", p.exportSettings.fps); ex->setProperty ("mbps", p.exportSettings.bitrateMbps);
    ex->setProperty ("codec", p.exportSettings.codec);
    root->setProperty ("export", juce::var (ex));
    return juce::var (root);
}

bool fromVar (const juce::var& v, Project& p, juce::String& error)
{
    if (v["format"].toString() != "InSequenceFX") { error = "Not an InSequence FX project"; return false; }
    if ((int) v["version"] > Project::kVersion) { error = "Project was made by a newer InSequence FX"; return false; }
    Project r;
    r.name = v["name"].toString(); r.active = (int) v["active"]; r.songId = v["songId"].toString(); r.plugins = v["plugins"];
    r.fps = v.hasProperty ("fps") ? (double) v["fps"] : 30.0;
    r.aspectW = v.hasProperty ("aspectW") ? (int) v["aspectW"] : 16;
    r.aspectH = v.hasProperty ("aspectH") ? (int) v["aspectH"] : 9;
    r.musicLock = (bool) v["musicLock"];
    if (auto* arr = v["media"].getArray())
        for (auto& m : *arr)
        {
            MediaItem mi;
            mi.id = m["id"].toString(); mi.path = m["path"].toString(); mi.name = m["name"].toString();
            mi.kind = (MediaKind) (int) m["kind"]; mi.duration = s2t (m["duration"]); mi.fps = (double) m["fps"];
            mi.width = (int) m["w"]; mi.height = (int) m["h"]; mi.rotation = (int) m["rot"]; mi.proxyPath = m["proxy"].toString(); mi.hasAudio = (bool) m["audio"];
            mi.favorite = (bool) m["fav"]; mi.quality = (float) (double) m["quality"];
            mi.sync.status = (SyncInfo::Status) (int) m["syncStatus"]; mi.sync.offset = s2t (m["syncOffset"]);
            mi.sync.confidence = (float) (double) m["syncConf"]; mi.sync.note = m["syncNote"].toString();
            r.media.push_back (mi);
        }
    auto a = v["analysis"];
    r.song.valid = (bool) a["valid"]; r.song.bpm = a.hasProperty ("bpm") ? (double) a["bpm"] : 120.0;
    r.song.gridOffset = s2t (a["offset"]); r.song.beatsPerBar = a.hasProperty ("bpb") ? (int) a["bpb"] : 4;
    r.song.duration = s2t (a["duration"]);
    if (auto* ev = a["events"].getArray())
        for (auto& e : *ev)
            r.song.events.push_back ({ s2t (e[0]), (EventType) (int) e[1], (float) (double) e[2], (float) (double) e[3] });
    if (auto* be = a["barEnergy"].getArray()) for (auto& x : *be) r.song.barEnergy.push_back ((float) (double) x);
    if (auto* bs = a["barSection"].getArray()) for (auto& x : *bs) r.song.barSection.push_back ((int) x);

    if (auto* arr = v["sequences"].getArray())
        for (auto& sv : *arr)
        {
            Sequence s;
            s.id = sv["id"].toString(); s.name = sv["name"].toString(); s.style = sv["style"].toString();
            s.seed = (juce::uint32) (int) sv["seed"]; fxFromVar (sv["fx"], s.fx);
            if (auto* tr = sv["tracks"].getArray())
                for (auto& tv : *tr)
                {
                    Track t; t.name = tv["name"].toString(); t.kind = (TrackKind) (int) tv["kind"];
                    t.mute = (bool) tv["mute"]; t.hidden = (bool) tv["hidden"]; t.solo = (bool) tv["solo"]; t.locked = (bool) tv["locked"];
                    t.height = tv.hasProperty ("height") ? (int) tv["height"] : 64;
                    t.id = tv["id"].toString(); t.plugins = tv["plugins"];
                    s.tracks.push_back (t);
                }
            if (auto* cl = sv["clips"].getArray())
                for (auto& cv : *cl)
                {
                    Clip c; c.id = cv["id"].toString(); c.mediaId = cv["media"].toString(); c.track = (int) cv["track"];
                    c.start = s2t (cv["start"]); c.length = s2t (cv["length"]); c.srcIn = s2t (cv["srcIn"]);
                    c.speed = cv.hasProperty ("speed") ? (double) cv["speed"] : 1.0; c.locked = (bool) cv["locked"];
                    c.group = (int) cv["group"]; c.opacity = cv.hasProperty ("opacity") ? (float) (double) cv["opacity"] : 1.0f;
                    c.volume = cv.hasProperty ("volume") ? (float) (double) cv["volume"] : 1.0f;
                    c.transIn = (TransitionType) (int) cv["trans"]; c.transLen = s2t (cv["transLen"]);
                    fxFromVar (cv["fx"], c.fx);
                    c.name = cv["name"].toString(); c.enabled = cv.hasProperty ("enabled") ? (bool) cv["enabled"] : true; c.reverse = (bool) cv["reverse"]; c.label = (int) cv["label"]; c.keep = (bool) cv["keep"];
                    if (c.track >= 0 && c.track < (int) s.tracks.size()) s.clips.push_back (c);
                }
            r.sequences.push_back (s);
        }
    if (r.sequences.empty()) r.sequences.push_back (Project::makeDefaultSequence ("Main Music Video"));
    auto ex = v["export"];
    if (ex.isObject())
    {
        r.exportSettings.width = (int) ex["w"]; r.exportSettings.height = (int) ex["h"];
        r.exportSettings.fps = (double) ex["fps"]; r.exportSettings.bitrateMbps = (int) ex["mbps"];
        r.exportSettings.codec = ex["codec"].toString();
    }
    p = std::move (r);
    return true;
}

bool saveProject (const Project& p, const juce::File& f, juce::String& error)
{
    auto json = juce::JSON::toString (toVar (p), false);
    juce::TemporaryFile tmp (f);
    if (! tmp.getFile().replaceWithText (json)) { error = "Could not write " + tmp.getFile().getFullPathName(); return false; }
    if (! tmp.overwriteTargetFileWithTemporary()) { error = "Could not replace " + f.getFullPathName(); return false; }
    return true;
}

bool loadProject (const juce::File& f, Project& p, juce::String& error)
{
    juce::var v;
    auto res = juce::JSON::parse (f.loadFileAsString(), v);
    if (res.failed()) { error = "Project file is damaged: " + res.getErrorMessage(); return false; }
    if (! fromVar (v, p, error)) return false;
    // relink media that moved together with the project folder
    for (auto& m : p.media)
    {
        juce::File mf (m.path);
        if (! mf.existsAsFile())
        {
            auto alt = f.getParentDirectory().getChildFile (mf.getFileName());
            if (alt.existsAsFile()) m.path = alt.getFullPathName();
            else m.missing = true;
        }
    }
    return true;
}

void History::push (const Project& p, const juce::String& what)
{
    undoStack.emplace_back (p, what);
    if (undoStack.size() > 200) undoStack.erase (undoStack.begin());
    redoStack.clear();
}
bool History::undo (Project& p)
{
    if (undoStack.empty()) return false;
    redoStack.emplace_back (p, undoStack.back().second);
    p = std::move (undoStack.back().first); undoStack.pop_back();
    return true;
}
bool History::redo (Project& p)
{
    if (redoStack.empty()) return false;
    undoStack.emplace_back (p, redoStack.back().second);
    p = std::move (redoStack.back().first); redoStack.pop_back();
    return true;
}
}
