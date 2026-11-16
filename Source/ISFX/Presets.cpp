#include "Presets.h"
#include "Editing.h"
#include <unordered_map>

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
    if (auto* ko = v["keys"].getDynamicObject())
        for (auto& pr : ko->getProperties())
            if (auto* arr = pr.value.getArray())
                for (auto& k : *arr)
                    if (auto* a = k.getArray(); a != nullptr && a->size() >= 3)
                        out.keys[pr.name.toString()].push_back ({ (Ticks) (juce::int64) (*a)[0], (float) (double) (*a)[1], (int) (*a)[2] });
    out.length = (Ticks) (juce::int64) v["length"]; out.atEnd = (bool) v["atEnd"];
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
    if (! p.keys.empty())
    {
        auto* ko = new juce::DynamicObject();
        for (auto& [id, ks] : p.keys)
        {
            juce::Array<juce::var> arr;
            for (auto& k : ks) arr.add (juce::Array<juce::var> { (juce::int64) k.t, k.v, k.interp });
            ko->setProperty (id, arr);
        }
        o->setProperty ("keys", juce::var (ko));
        o->setProperty ("length", (juce::int64) p.length); o->setProperty ("atEnd", p.atEnd);
    }
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

// ------------------------------------------------------------------ Premiere .prfpset import
namespace
{
struct PrObj { std::string tag; size_t a = 0, b = 0; };

juce::String xmlUnescape (const std::string& s)
{
    juce::String r = juce::String::fromUTF8 (s.data(), (int) s.size());
    return r.replace ("&lt;", "<").replace ("&gt;", ">").replace ("&quot;", "\"").replace ("&apos;", "'").replace ("&amp;", "&");
}

struct PrDoc
{
    std::string s;
    std::unordered_map<std::string, PrObj> objs;

    void index()
    {
        size_t p = 0;
        const std::string key = " ObjectID=\"";
        while ((p = s.find (key, p)) != std::string::npos)
        {
            size_t lt = s.rfind ('<', p);
            size_t q = s.find ('"', p + key.size());
            if (lt == std::string::npos || q == std::string::npos) break;
            PrObj o; o.tag = s.substr (lt + 1, p - lt - 1);
            const std::string id = s.substr (p + key.size(), q - p - key.size());
            o.a = s.find ('>', q); if (o.a == std::string::npos) break;
            const std::string close = "</" + o.tag + ">";
            o.b = s.find (close, o.a); if (o.b == std::string::npos) break;
            objs[id] = o; p = o.a;
        }
    }
    const PrObj* get (const std::string& id) const { auto it = objs.find (id); return it == objs.end() ? nullptr : &it->second; }
    // first <tag>value</tag> inside [a,b)
    std::string field (const PrObj& o, const char* tag) const
    {
        const std::string open = std::string ("<") + tag + ">", close = std::string ("</") + tag + ">";
        size_t x = s.find (open, o.a); if (x == std::string::npos || x >= o.b) return {};
        x += open.size(); size_t y = s.find (close, x); if (y == std::string::npos || y > o.b) return {};
        return s.substr (x, y - x);
    }
    // all ObjectRef="n" of elements <elem ... ObjectRef="n"/> inside the object
    std::vector<std::string> refs (const PrObj& o, const char* elem) const
    {
        std::vector<std::string> r;
        const std::string open = std::string ("<") + elem + " ";
        for (size_t x = s.find (open, o.a); x != std::string::npos && x < o.b; x = s.find (open, x + 1))
        {
            size_t e = s.find ('>', x), k = s.find ("ObjectRef=\"", x);
            if (k == std::string::npos || k > e) continue;
            k += 11; r.push_back (s.substr (k, s.find ('"', k) - k));
        }
        return r;
    }
};

constexpr double kPrTicks = 254016000000.0;   // Premiere ticks per second

// one Premiere value token: "12." or "0.5:0.5" (point, normalized)
bool prValue (const juce::String& tok, float& x, float& y, bool& point)
{
    point = tok.containsChar (':');
    if (point) { x = tok.upToFirstOccurrenceOf (":", false, false).getFloatValue(); y = tok.fromFirstOccurrenceOf (":", false, false).getFloatValue(); return true; }
    if (tok.isEmpty() || tok.containsOnly ("-0123456789.eE+") == false) return false;
    x = y = tok.getFloatValue(); return true;
}

struct PrParam { juce::String name; bool animated = false, point = false; float x = 0, y = 0; std::vector<std::tuple<double, float, float>> keys; };

PrParam readParam (const PrDoc& d, const PrObj& o, double anchor)
{
    PrParam r;
    r.name = xmlUnescape (d.field (o, "Name"));
    r.animated = d.field (o, "IsTimeVarying") == "true";
    auto start = juce::StringArray::fromTokens (xmlUnescape (d.field (o, "StartKeyframe")), ",", "");
    bool pt = false;
    if (start.size() > 1 && prValue (start[1], r.x, r.y, pt)) r.point = pt;
    else { auto cur = xmlUnescape (d.field (o, "CurrentValue")); if (prValue (cur.replaceCharacter (',', ':'), r.x, r.y, pt)) r.point = pt && cur.containsChar (','); }
    if (r.animated)
        for (auto& k : juce::StringArray::fromTokens (xmlUnescape (d.field (o, "Keyframes")), ";", ""))
        {
            auto t = juce::StringArray::fromTokens (k, ",", "");
            float x = 0, y = 0; bool p2 = false;
            if (t.size() < 2 || ! prValue (t[1], x, y, p2)) continue;
            r.point |= p2;
            r.keys.emplace_back ((t[0].getLargeIntValue() - anchor) / kPrTicks, x, y);
        }
    if (r.keys.empty()) r.animated = false;
    return r;
}

struct Mapper
{
    UserPreset& up;
    juce::StringArray& missing;
    void put (const juce::String& id, const PrParam& p, std::function<float (float, float)> f)
    {
        if (p.animated)
        {
            auto& ks = up.keys[id]; ks.clear();
            for (auto& [sec, x, y] : p.keys) ks.push_back ({ secToTicks (std::max (0.0, sec)), f (x, y), 2 });
            std::sort (ks.begin(), ks.end(), [] (auto& a, auto& b) { return a.t < b.t; });
            up.fx.set (id, ks.front().v);
        }
        else up.fx.set (id, f (p.x, p.y));
    }
};
}

static bool mapEffect (const juce::String& match, const juce::String& display, const std::vector<PrParam>& ps, UserPreset& up, juce::StringArray& missing)
{
    Mapper m { up, missing };
    auto pct = [] (float x, float) { return x / 100.0f; };
    auto con = [] (float x, float) { return 1.0f + x / 100.0f; };
    auto lim = [] (float lo, float hi) { return [lo, hi] (float x, float) { return juce::jlimit (lo, hi, x); }; };
    std::map<juce::String, std::function<void (const PrParam&)>> table;
    juce::StringArray ignore;   // non-visual or covered by another parameter
    const bool transform = match == "AE.ADBE Geometry2" || match == "AE.ADBE Geometry";
    if (transform)
    {
        table["Scale"] = [&] (auto& p) { m.put ("scale", p, [] (float x, float) { return juce::jlimit (0.1f, 4.0f, x / 100.0f); }); };
        table["Scale Height"] = [&] (auto& p) { m.put ("scaleY", p, [] (float x, float) { return juce::jlimit (0.1f, 4.0f, x / 100.0f); }); };
        table["Scale Width"] = [&] (auto& p) { m.put ("scaleX", p, [] (float x, float) { return juce::jlimit (0.1f, 4.0f, x / 100.0f); }); };
        table["Position"] = [&] (auto& p) { if (! p.point) return; m.put ("posX", p, [] (float x, float) { return juce::jlimit (-1.0f, 1.0f, (x - 0.5f) * 2); });
                                                               m.put ("posY", p, [] (float, float y) { return juce::jlimit (-1.0f, 1.0f, (y - 0.5f) * 2); }); };
        table["Rotation"] = [&] (auto& p) { m.put ("rotation", p, lim (-180, 180)); };
        table["Opacity"] = [&] (auto& p) { m.put ("opacity", p, pct); };
        ignore.addArray ({ "Uniform Scale", "Use Composition's Shutter Angle", "Sampling", "Anchor Point" });
    }
    else if (match == "AE.ADBE Opacity") { table["Opacity"] = [&] (auto& p) { m.put ("opacity", p, pct); }; ignore.add ("Blend Mode"); }
    else if (match == "AE.ADBE Lumetri")
    {
        table["Exposure"] = [&] (auto& p) { m.put ("exposure", p, lim (-2, 2)); };
        table["Contrast"] = [&] (auto& p) { m.put ("contrast", p, con); };
        table["Saturation"] = [&] (auto& p) { m.put ("saturation", p, [] (float x, float) { return juce::jlimit (0.0f, 2.0f, x / 100.0f); }); };
        table["Temperature"] = [&] (auto& p) { m.put ("temperature", p, pct); };
        table["Tint"] = [&] (auto& p) { m.put ("tint", p, pct); };
        table["Highlights"] = [&] (auto& p) { m.put ("highlights", p, pct); };
        table["Shadows"] = [&] (auto& p) { m.put ("shadows", p, pct); };
    }
    else if (match == "PR.ADBE Lens Distortion") table["Curvature"] = [&] (auto& p) { m.put ("lensCurve", p, pct); };
    else if (match == "AE.ADBE Gaussian Blur 2" || match == "AE.ADBE Fast Blur")
    {
        table["Blurriness"] = [&] (auto& p) { m.put ("blur", p, [] (float x, float) { return juce::jlimit (0.0f, 1.0f, x / 200.0f); }); };
        ignore.addArray ({ "Blur Dimensions", "Repeat Edge Pixels" });
    }
    else if (match == "AE.ADBE Posterize Time") table["Frame Rate"] = [&] (auto& p) { m.put ("posterizeTime", p, lim (0, 60)); };
    else if (match == "AE.ADBE Brightness & Contrast 2")
    {
        table["Brightness"] = [&] (auto& p) { m.put ("brightness", p, pct); };
        table["Contrast"] = [&] (auto& p) { m.put ("contrast", p, con); };
        ignore.add ("Use Legacy");
    }
    else if (match == "AE.ADBE Invert") { table["Blend With Original"] = [&] (auto& p) { m.put ("invert", p, [] (float x, float) { return 1.0f - x / 100.0f; }); }; ignore.add ("Channel"); }
    else if (match == "AE.ADBE PPro Timecode") return true;
    if (table.empty() && ! ps.empty()) { missing.addIfNotAlreadyThere (display.isNotEmpty() ? display : match); return false; }
    if (match == "AE.ADBE Invert" && up.fx.p.count ("invert") == 0) up.fx.set ("invert", 1.0f);
    for (auto& p : ps)
    {
        if (auto it = table.find (p.name); it != table.end()) it->second (p);
        else if (p.animated && ! ignore.contains (p.name)) missing.addIfNotAlreadyThere (display + ": " + p.name);
    }
    return true;
}

int importPrfpset (const juce::File& file, const juce::File& outDir, juce::StringArray& missing, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb)) { error = "Cannot read " + file.getFileName(); return 0; }
    PrDoc d; d.s.assign ((const char*) mb.getData(), mb.getSize()); mb.reset();
    if (d.s.find ("<PremiereData") == std::string::npos) { error = file.getFileName() + " is not a Premiere preset file"; return 0; }
    d.index();
    const auto pack = file.getFileNameWithoutExtension().replaceCharacter ('_', ' ').trim();
    std::unordered_map<std::string, std::string> parent;   // tree item -> bin
    for (auto& [id, o] : d.objs)
        if (o.tag == "BinTreeItem")
            for (auto& c : d.refs (o, "Item")) parent[c] = id;
    auto dir = outDir == juce::File() ? userDataDir().getChildFile ("Presets") : outDir;
    juce::StringArray used;
    dir.createDirectory();
    int n = 0;
    for (auto& [id, o] : d.objs)
    {
        if (o.tag != "TreeItem") continue;
        auto data = d.refs (o, "Data");
        const PrObj* item = data.empty() ? nullptr : d.get (data[0]);
        if (item == nullptr || item->tag != "FilterPresetItem") continue;
        UserPreset up; up.clip = true;
        juce::StringArray path { xmlUnescape (d.field (o, "Name")) };
        for (auto b = parent.find (id); b != parent.end(); b = parent.find (b->second))
            if (auto* bo = d.get (b->second)) { auto nm = xmlUnescape (d.field (*bo, "Name")); if (nm != "Root" && nm != "Presets" && nm.isNotEmpty()) path.insert (0, nm); }
        path.insert (0, pack);
        up.name = path.joinIntoString (" / ");
        double len = 0; bool any = false;
        for (auto& fpId : d.refs (*item, "FilterPreset"))
        {
            auto* fp = d.get (fpId); if (fp == nullptr) continue;
            const double in = (double) juce::String (d.field (*fp, "AnchorInPoint")).getLargeIntValue();
            const double out = (double) juce::String (d.field (*fp, "AnchorOutPoint")).getLargeIntValue();
            len = std::max (len, (out - in) / kPrTicks);
            auto comp = d.refs (*fp, "Component");
            auto* co = comp.empty() ? nullptr : d.get (comp[0]);
            if (co == nullptr) continue;
            if (co->tag != "VideoFilterComponent") continue;
            const auto match = xmlUnescape (d.field (*co, "MatchName")), display = xmlUnescape (d.field (*co, "DisplayName"));
            std::vector<PrParam> ps;
            for (auto& pid : d.refs (*co, "Param")) if (auto* po = d.get (pid)) ps.push_back (readParam (d, *po, in));
            any |= mapEffect (match, display, ps, up, missing);
        }
        if (! any) continue;
        up.length = secToTicks (len);
        const auto lower = up.name.toLowerCase();
        up.atEnd = lower.contains (" out") || lower.endsWith ("out") || lower.contains ("outro");
        juce::File written; juce::String e;
        UserPreset w = up;
        auto base = juce::File::createLegalFileName (up.name.replace (" / ", " - "));
        auto fname = base;
        for (int k = 2; used.contains (fname.toLowerCase()); ++k) fname = base + " " + juce::String (k);   // same-named presets in one pack
        used.add (fname.toLowerCase());
        w.file = dir.getChildFile (fname + ".isfxpreset");
        if (saveUserPreset (w, written, e, dir)) ++n;
    }
    if (n == 0 && error.isEmpty()) error = "No video effects in " + file.getFileName() + " could be rebuilt";
    return n;
}

void applyUserPreset (Clip& c, const UserPreset& u)
{
    c.fx = u.fx;
    for (auto it = c.keys.begin(); it != c.keys.end();) it = it->first == "speed" ? std::next (it) : c.keys.erase (it);
    const Ticks off = u.atEnd && u.length > 0 ? std::max<Ticks> (0, c.length - u.length) : 0;
    for (auto& [id, ks] : u.keys)
    {
        auto& dst = c.keys[id]; dst = ks;
        for (auto& k : dst) k.t += off;
    }
}


juce::String importPresetFiles (const juce::Array<juce::File>& files, int& imported, const juce::File& rootIn)
{
    const auto root = rootIn == juce::File() ? userDataDir() : rootIn;
    juce::StringArray lines, missing, aeFiles;
    imported = 0;
    for (auto& f : files)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        auto copyTo = [&] (const char* sub) { auto d = root.getChildFile (sub); d.createDirectory(); return f.copyFileTo (d.getChildFile (f.getFileName())); };
        if (ext == ".prfpset")
        {
            juce::String err; juce::StringArray miss;
            const int n = importPrfpset (f, root.getChildFile ("Presets"), miss, err);
            imported += n;
            lines.add (f.getFileNameWithoutExtension() + ": " + (n > 0 ? juce::String (n) + " presets" : err));
            for (auto& m : miss) missing.addIfNotAlreadyThere (m);
        }
        else if (ext == ".cube") { if (copyTo ("LUTs")) { ++imported; lines.add (f.getFileName() + ": LUT"); } }
        else if (ext == ".isfxpreset") { if (copyTo ("Presets")) { ++imported; lines.add (f.getFileName() + ": preset"); } }
        else if (ext == ".isfxtemplate") { if (copyTo ("Templates")) { ++imported; lines.add (f.getFileName() + ": template"); } }
        else if (ext == ".ffx") { copyTo ("Presets/After Effects"); aeFiles.add (f.getFileName()); }
        else lines.add (f.getFileName() + ": not a preset format");
    }
    juce::String r = lines.joinIntoString ("\n");
    if (! aeFiles.isEmpty())
        r << "\n\nAfter Effects .ffx presets only run inside After Effects. Kept in Presets/After Effects so they can be rebuilt natively: " << aeFiles.joinIntoString (", ");
    if (! missing.isEmpty())
        r << "\n\nNot rebuilt yet (left out, everything else kept): " << missing.joinIntoString (", ");
    return r;
}
}
