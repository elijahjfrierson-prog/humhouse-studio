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
        if (! m.moments.empty()) { juce::String mo; for (auto v : m.moments) mo << juce::String (v, 2) << ","; o->setProperty ("moments", mo); }
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
        if (s.duckDb < 0) { o->setProperty ("duckDb", s.duckDb); o->setProperty ("duckKey", s.duckKey); }
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
            if (! c.keys.empty())
            {
                auto* ko = new juce::DynamicObject();
                for (auto& [id, ks] : c.keys)
                {
                    juce::Array<juce::var> a;
                    for (auto& k : ks) a.add (juce::Array<juce::var> { t2s (k.t), k.v, k.interp });
                    ko->setProperty (id, a);
                }
                co->setProperty ("keys", juce::var (ko));
            }
            if (c.name.isNotEmpty()) co->setProperty ("name", c.name);
            if (! c.enabled) co->setProperty ("enabled", false);
            if (c.reverse) co->setProperty ("reverse", true);
            if (c.label != 0) co->setProperty ("label", c.label);
            if (c.keep) co->setProperty ("keep", true);
            if (c.frameInterp != 1) co->setProperty ("frameInterp", c.frameInterp);
            if (c.fadeIn > 0) co->setProperty ("fadeIn", t2s (c.fadeIn));
            if (c.fadeOut > 0) co->setProperty ("fadeOut", t2s (c.fadeOut));
            if (c.text.isNotEmpty()) { co->setProperty ("text", c.text); co->setProperty ("font", c.font); }
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
            for (auto& tk : juce::StringArray::fromTokens (m["moments"].toString(), ",", "")) if (tk.isNotEmpty()) mi.moments.push_back (tk.getFloatValue());
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
            s.duckDb = (float) (double) sv["duckDb"]; s.duckKey = sv["duckKey"].toString();
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
                    if (auto* ko = cv["keys"].getDynamicObject())
                        for (auto& pr : ko->getProperties())
                            if (auto* a = pr.value.getArray())
                                for (auto& k : *a)
                                    if (k.size() >= 2) setKey (c, pr.name.toString(), s2t (k[0]), (float) (double) k[1], k.size() > 2 ? (int) k[2] : 0);
                    c.name = cv["name"].toString(); c.enabled = cv.hasProperty ("enabled") ? (bool) cv["enabled"] : true; c.reverse = (bool) cv["reverse"]; c.label = (int) cv["label"]; c.keep = (bool) cv["keep"];
                    c.frameInterp = cv.hasProperty ("frameInterp") ? (int) cv["frameInterp"] : 1;
                    c.fadeIn = cv.hasProperty ("fadeIn") ? s2t (cv["fadeIn"]) : 0;
                    c.fadeOut = cv.hasProperty ("fadeOut") ? s2t (cv["fadeOut"]) : 0;
                    c.text = cv["text"].toString(); c.font = cv["font"].toString();
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
float volumeGain (const Clip& c, Ticks rel)
{
    float g = c.volume;
    if (auto it = c.keys.find ("volume"); it != c.keys.end() && ! it->second.empty()) g = keyValue (it->second, rel);
    if (c.fadeIn > 0 && rel < c.fadeIn) g *= (float) std::max<Ticks> (0, rel) / (float) c.fadeIn;
    if (c.fadeOut > 0 && c.length - rel < c.fadeOut) g *= (float) std::max<Ticks> (0, c.length - rel) / (float) c.fadeOut;
    return g;
}

float keyValue (const std::vector<Clip::Key>& k, Ticks rel)
{
    if (k.empty()) return 0;
    if (rel <= k.front().t) return k.front().v;
    if (rel >= k.back().t) return k.back().v;
    auto it = std::upper_bound (k.begin(), k.end(), rel, [] (Ticks x, const Clip::Key& y) { return x < y.t; });
    const auto& b = *it; const auto& a = *(it - 1);
    if (a.interp == 1 || b.t <= a.t) return a.v;
    double u = (double) (rel - a.t) / (double) (b.t - a.t);
    if (a.interp == 2) u = u * u * (3 - 2 * u);
    return (float) (a.v + (b.v - a.v) * u);
}

void applyKeyframes (Clip& c, Ticks t)
{
    for (auto& [id, ks] : c.keys) if (! ks.empty()) c.fx.set (id, keyValue (ks, t - c.start));
}

static double rampSeg (const Clip::Key& a, const Clip::Key& b, double x)
{
    const double d = (double) (b.t - a.t);
    if (d <= 0 || a.interp == 1) return std::max (0.0f, a.v) * x;
    const double v0 = std::max (0.0f, a.v), dv = std::max (0.0f, b.v) - v0, u = x / d;
    if (a.interp == 2) return v0 * x + dv * d * (u * u * u - u * u * u * u / 2);
    return v0 * x + dv * x * u / 2;
}

static double rampedRaw (const Clip& c, Ticks rel);
double rampedOffset (const Clip& c, Ticks rel)
{
    // keys can sit before 0 after a split; integrate speed from the clip start only
    return rampedRaw (c, rel) - rampedRaw (c, 0);
}

static double rampedRaw (const Clip& c, Ticks rel)
{
    auto it = c.keys.find ("speed");
    if (it == c.keys.end() || it->second.empty()) return (double) rel * c.speed;
    const auto& k = it->second;
    if (rel <= k.front().t) return (double) rel * std::max (0.0f, k.front().v);
    double acc = (double) k.front().t * std::max (0.0f, k.front().v);
    for (size_t i = 0; i + 1 < k.size(); ++i)
    {
        if (rel <= k[i + 1].t) return acc + rampSeg (k[i], k[i + 1], (double) (rel - k[i].t));
        acc += rampSeg (k[i], k[i + 1], (double) (k[i + 1].t - k[i].t));
    }
    return acc + (double) (rel - k.back().t) * std::max (0.0f, k.back().v);
}

void shiftKeys (Clip& c, Ticks delta)
{
    for (auto& [id, ks] : c.keys) for (auto& k : ks) k.t -= delta;
}

void setKey (Clip& c, const juce::String& param, Ticks rel, float v, int interp)
{
    auto& ks = c.keys[param];
    for (auto& k : ks) if (k.t == rel) { k.v = v; k.interp = interp; return; }
    ks.push_back ({ rel, v, interp });
    std::sort (ks.begin(), ks.end(), [] (const Clip::Key& a, const Clip::Key& b) { return a.t < b.t; });
}
}
