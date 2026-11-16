#include <array>
#include "Editing.h"
#include "Looks.h"
#include <algorithm>
#include <set>

namespace isfx
{
juce::StringArray snapModeNames() { return { "Off", "Bar", "Beat", "1/2", "1/4", "1/8", "1/16", "1/32", "Triplet", "Frame", "1 s", "100 ms", "10 ms" }; }

// beat = quarter note. 1/8 note = 2 per beat, 1/16 = 4, 1/32 = 8
int snapPerBeat (SnapMode m)
{
    switch (m)
    {
        case SnapMode::Beat: return 1;
        case SnapMode::Half: return 1;     // handled as 2 beats
        case SnapMode::Quarter: return 1;
        case SnapMode::Eighth: return 2;
        case SnapMode::Sixteenth: return 4;
        case SnapMode::ThirtySecond: return 8;
        case SnapMode::Triplet: return 3;
        default: return 0;
    }
}

Ticks snapTime (Ticks t, const Project& p, SnapMode m)
{
    if (m == SnapMode::Off) return t;
    if (m == SnapMode::Sec1 || m == SnapMode::Ms100 || m == SnapMode::Ms10)
    {
        const Ticks step = secToTicks (m == SnapMode::Sec1 ? 1.0 : m == SnapMode::Ms100 ? 0.1 : 0.01);
        return (t + step / 2) / step * step;
    }
    if (m == SnapMode::Frame || ! p.song.valid) return quantizeToFrame (t, p.fps);
    const auto& s = p.song;
    double beats = s.beatAt (t);
    if (m == SnapMode::Bar)  return s.gridTime (std::round (beats / s.beatsPerBar) * s.beatsPerBar);
    if (m == SnapMode::Half) return s.gridTime (std::round (beats / 2.0) * 2.0);
    int per = snapPerBeat (m);
    return s.subdivTime ((juce::int64) std::llround (beats * per), per);
}

juce::StringArray chopModeNames() { return { "CHOP 1/8", "CHOP 1/16", "CHOP 1/32", "KICK", "SNARE", "HI-HAT" }; }

std::vector<Ticks> hatRolls (const SongAnalysis& s, Ticks a, Ticks b)
{
    std::vector<Ticks> hats, out;
    // detect rolls over the whole song so a roll straddling a clip boundary still counts, then keep the hits inside [a, b)
    for (auto& e : s.events) if (e.type == EventType::HiHat) hats.push_back (e.time);
    std::sort (hats.begin(), hats.end());
    const Ticks gap = secToTicks (s.beatSec() / 3.0 + 0.01), minLen = secToTicks (1.0 / 15.0);
    for (size_t i = 0; i < hats.size();)
    {
        size_t j = i;
        while (j + 1 < hats.size() && hats[j + 1] - hats[j] <= gap) ++j;
        if (j - i + 1 >= 4)
        {
            Ticks last = hats[i] - minLen * 2;
            for (size_t k = i; k <= j; ++k)
                if (hats[k] - last >= minLen) { if (hats[k] >= a && hats[k] < b) out.push_back (hats[k]); last = hats[k]; }
        }
        i = j + 1;
    }
    return out;
}

std::vector<Ticks> chopPoints (const SongAnalysis& s, Ticks a, Ticks b, ChopMode m)
{
    std::vector<Ticks> out;
    if (b <= a) return out;
    if (m == ChopMode::HatRoll) return hatRolls (s, a, b);
    if (m == ChopMode::Grid8 || m == ChopMode::Grid16 || m == ChopMode::Grid32)
    {
        int per = m == ChopMode::Grid8 ? 2 : (m == ChopMode::Grid16 ? 4 : 8);
        auto n0 = (juce::int64) std::ceil (s.beatAt (a) * per - 1e-6);
        for (auto n = n0;; ++n)
        {
            auto t = s.subdivTime (n, per);
            if (t >= b) break;
            if (t >= a) out.push_back (t);
        }
        return out;
    }
    EventType want = m == ChopMode::Kick ? EventType::Kick : (m == ChopMode::Snare ? EventType::Snare : EventType::HiHat);
    for (auto& e : s.events) if (e.type == want && e.time >= a && e.time < b) out.push_back (e.time);
    return out;
}

std::vector<const MediaItem*> takesCovering (const Project& p, Ticks t, Ticks len)
{
    std::vector<const MediaItem*> r;
    for (auto& m : p.media)
    {
        if (m.kind != MediaKind::Video || m.missing) continue;
        if (m.sync.status != SyncInfo::Synced && m.sync.status != SyncInfo::Manual) continue;
        if (m.sync.offset <= t && m.sync.offset + m.duration >= t + len) r.push_back (&m);
    }
    return r;
}

// ------------------------------------------------------------------ basic edits
bool splitAt (Sequence& s, Ticks t, const juce::StringArray& only, const juce::String& songId)
{
    bool any = false;
    std::vector<Clip> added;
    for (auto& c : s.clips)
    {
        const bool song = songId.isNotEmpty() && c.mediaId == songId;
        if ((c.locked && ! song) || s.tracks[(size_t) c.track].locked) continue;
        if (! only.isEmpty() && ! only.contains (c.id)) continue;
        if (t <= c.start || t >= c.end()) continue;
        Clip r = c;
        r.id = newId();
        if (c.reverse) { r.srcIn = c.srcIn; c.srcIn = c.srcAt (t) + 1; }   // reversed: the left half holds the later source
        else r.srcIn = c.srcAt (t);
        shiftKeys (r, t - c.start);
        r.start = t; r.length = c.end() - t;
        r.transIn = TransitionType::Cut; r.transLen = 0;
        c.length = t - c.start;
        added.push_back (r); any = true;
    }
    for (auto& a : added) s.clips.push_back (a);
    s.sortClips();
    return any;
}

void deleteClips (Sequence& s, const juce::StringArray& ids, bool ripple)
{
    std::vector<Clip> removed;
    s.clips.erase (std::remove_if (s.clips.begin(), s.clips.end(), [&] (const Clip& c)
    {
        bool del = ids.contains (c.id) && ! c.locked && ! s.tracks[(size_t) c.track].locked;
        if (del) removed.push_back (c);
        return del;
    }), s.clips.end());
    if (! ripple) return;
    // close the gaps per track, latest first so positions stay valid
    std::sort (removed.begin(), removed.end(), [] (auto& a, auto& b) { return a.start > b.start; });
    for (auto& r : removed)
        for (auto& c : s.clips)
            if (c.track == r.track && c.start >= r.end() && ! c.locked) c.start -= r.length;
}

void clearTrackRange (Sequence& s, int track, Ticks a, Ticks b)
{
    std::vector<Clip> keep;
    for (auto& c : s.clips)
    {
        if (c.track != track || c.end() <= a || c.start >= b) { keep.push_back (c); continue; }
        if (c.start < a)
        {
            Clip l = c; l.length = a - c.start; if (c.reverse) l.srcIn = c.srcAt (a) + 1; keep.push_back (l);
        }
        if (c.end() > b)
        {
            Clip r = c; r.id = newId(); r.srcIn = c.reverse ? c.srcIn : c.srcAt (b); r.start = b; r.length = c.end() - b;
            r.transIn = TransitionType::Cut; r.transLen = 0; keep.push_back (r);
        }
    }
    s.clips = keep;
    s.sortClips();
}

void deleteRange (Sequence& s, Ticks a, Ticks b, bool ripple, int track)
{
    for (int t = 0; t < (int) s.tracks.size(); ++t)
    {
        if (track >= 0 && t != track) continue;
        if (s.tracks[(size_t) t].locked) continue;
        clearTrackRange (s, t, a, b);
        if (ripple)
            for (auto& c : s.clips) if (c.track == t && c.start >= b) c.start -= (b - a);
    }
}

void keepRange (Sequence& s, Ticks a, Ticks b)
{
    for (int t = 0; t < (int) s.tracks.size(); ++t)
    {
        if (s.tracks[(size_t) t].kind == TrackKind::Audio) continue;   // song stays (Music Lock)
        clearTrackRange (s, t, std::numeric_limits<Ticks>::min() / 2, a);
        clearTrackRange (s, t, b, std::numeric_limits<Ticks>::max() / 2);
    }
}

int insertVideoTrack (Sequence& s)
{
    int at = 0, n = 0;
    for (int i = 0; i < (int) s.tracks.size(); ++i) if (s.tracks[(size_t) i].kind == TrackKind::Video) { at = i + 1; ++n; }
    Track t; t.name = "V" + juce::String (n + 1); t.kind = TrackKind::Video;
    s.tracks.insert (s.tracks.begin() + at, t);
    for (auto& c : s.clips) if (c.track >= at) ++c.track;
    return at;
}

int freeVideoTrack (const Sequence& s, int prefer, Ticks a, Ticks b)
{
    const int n = (int) s.tracks.size();
    auto freeOn = [&] (int k)
    {
        if (k < 0 || k >= n || s.tracks[(size_t) k].kind != TrackKind::Video || s.tracks[(size_t) k].locked) return false;
        for (auto& c : s.clips) if (c.track == k && c.start < b && c.end() > a) return false;
        return true;
    };
    if (prefer < 0 || prefer >= n || s.tracks[(size_t) prefer].kind != TrackKind::Video)
    {   // no target: the top video layer if it has room
        int top = -1; for (int k = 0; k < n; ++k) if (s.tracks[(size_t) k].kind == TrackKind::Video) top = k;
        return freeOn (top) ? top : -1;
    }
    for (int k = prefer; k < n; ++k) if (freeOn (k)) return k;   // where it was dropped, else the nearest room above it
    return -1;
}

juce::String addEffectClip (Sequence& s, const FxStack& fx, const juce::String& name, Ticks at, Ticks len, int preferTrack)
{
    if (len <= 0) return {};
    int tr = freeVideoTrack (s, preferTrack, std::max<Ticks> (0, at), std::max<Ticks> (0, at) + len);
    if (tr < 0) tr = insertVideoTrack (s);
    Clip c; c.id = newId(); c.track = tr; c.start = std::max<Ticks> (0, at); c.length = len; c.fx = fx; c.name = name;
    s.clips.push_back (c); s.sortClips();
    return c.id;
}

juce::String addEffectClipOn (Sequence& s, int track, const FxStack& fx, const juce::String& name, Ticks at, Ticks len)
{
    if (len <= 0) return {};
    at = std::max<Ticks> (0, at);
    bool free = track >= 0 && track < (int) s.tracks.size() && s.tracks[(size_t) track].kind == TrackKind::Video && ! s.tracks[(size_t) track].locked;
    for (auto& c : s.clips)
        if (free && c.track == track && c.start < at + len && c.end() > at) free = false;
    if (! free) return addEffectClip (s, fx, name, at, len);
    Clip c; c.id = newId(); c.track = track; c.start = at; c.length = len; c.fx = fx; c.name = name;
    s.clips.push_back (c); s.sortClips();
    return c.id;
}

bool setTransitionAtCut (Sequence& s, int track, Ticks at, TransitionType ty, Ticks len, Ticks maxDist)
{
    if (track < 0 || track >= (int) s.tracks.size() || s.tracks[(size_t) track].locked) return false;
    Clip* best = nullptr; Ticks bestD = maxDist + 1;
    for (auto& c : s.clips)
        if (c.track == track && ! c.locked)
        {
            Ticks d = std::abs (c.start - at);
            if (d < bestD) { bestD = d; best = &c; }
        }
    if (best == nullptr) return false;
    best->transIn = ty;
    if (ty == TransitionType::Cut) best->transLen = 0;
    else best->transLen = juce::jlimit<Ticks> (1, std::max<Ticks> (1, best->length), len);
    return true;
}

bool shiftSong (Project& p, Ticks delta, bool withSynced)
{
    if (p.songId.isEmpty() || delta == 0) return false;
    Ticks minStart = std::numeric_limits<Ticks>::max();
    for (auto& sq : p.sequences) for (auto& c : sq.clips) if (c.mediaId == p.songId) minStart = std::min (minStart, c.start);
    if (minStart == std::numeric_limits<Ticks>::max()) return false;
    delta = std::max (delta, -minStart);
    if (delta == 0) return false;
    auto synced = [&] (const Clip& c)
    {
        auto* m = p.findMedia (c.mediaId);
        return m != nullptr && c.speed == 1.0 && (m->sync.status == SyncInfo::Synced || m->sync.status == SyncInfo::Manual);
    };
    for (auto& sq : p.sequences)
    {
        for (auto& c : sq.clips)
            if (c.mediaId == p.songId || (withSynced && synced (c))) c.start = std::max<Ticks> (0, c.start + delta);
        sq.sortClips();
    }
    if (withSynced)
    {
        for (auto& m : p.media) if (m.sync.status == SyncInfo::Synced || m.sync.status == SyncInfo::Manual) m.sync.offset += delta;
        p.song.gridOffset += delta;
        for (auto& e : p.song.events) e.time += delta;
    }
    return true;
}

void settleClips (Sequence& s, const juce::StringArray& ids)
{
    std::vector<Clip> moved;
    for (auto& c : s.clips) if (ids.contains (c.id)) moved.push_back (c);
    s.clips.erase (std::remove_if (s.clips.begin(), s.clips.end(), [&] (const Clip& c) { return ids.contains (c.id); }), s.clips.end());
    for (auto& m : moved) clearTrackRange (s, m.track, m.start, m.end());
    for (auto& m : moved) s.clips.push_back (m);
    s.sortClips();
}

void fillGaps (Sequence& s, const Project& p, int track)
{
    std::vector<Clip*> v;
    for (auto& c : s.clips) if (c.track == track) v.push_back (&c);
    std::sort (v.begin(), v.end(), [] (const Clip* a, const Clip* b) { return a->start < b->start; });
    for (size_t i = 0; i + 1 < v.size(); ++i)
    {
        Clip& a = *v[i]; Clip& b = *v[i + 1];
        Ticks gap = b.start - a.end();
        if (gap <= 0) continue;
        auto* ma = p.findMedia (a.mediaId);
        Ticks roomA = (ma == nullptr || ma->kind == MediaKind::Image) ? gap
                    : (a.speed <= 0 ? gap : (Ticks) ((double) (ma->duration - a.srcAt (a.end())) / a.speed));
        Ticks ext = std::min (gap, std::max<Ticks> (0, roomA));
        if (! a.locked) { a.length += ext; gap -= ext; }
        if (gap > 0 && ! b.locked)
        {
            Ticks e2 = b.speed <= 0 ? gap : std::min (gap, std::max<Ticks> (0, (Ticks) ((double) b.srcIn / b.speed)));
            shiftKeys (b, -e2); b.start -= e2; b.length += e2; b.srcIn -= (Ticks) ((double) e2 * b.speed);
        }
    }
}

bool moveClips (Sequence& s, const juce::StringArray& ids, Ticks delta, int trackDelta)
{
    for (auto& c : s.clips)
    {
        if (! ids.contains (c.id)) continue;
        int nt = c.track + trackDelta;
        if (c.locked || nt < 0 || nt >= (int) s.tracks.size()) return false;
        if (s.tracks[(size_t) nt].kind != s.tracks[(size_t) c.track].kind || s.tracks[(size_t) nt].locked) return false;
        if (c.start + delta < 0) return false;
    }
    for (auto& c : s.clips) if (ids.contains (c.id)) { c.start += delta; c.track += trackDelta; }
    s.sortClips();
    return true;
}

bool trimClip (Sequence& s, const Project& p, const juce::String& id, bool left, Ticks e)
{
    auto* c = s.findClip (id);
    if (c == nullptr || (c->locked && (p.songId.isEmpty() || c->mediaId != p.songId))) return false;   // the song is trimmable
    auto* m = p.findMedia (c->mediaId);
    Ticks mediaLen = m ? m->duration : std::numeric_limits<Ticks>::max() / 4;
    Ticks minLen = secToTicks (1.0 / p.fps);
    if (left)
    {
        Ticks newSrc = c->srcIn + (Ticks) ((double) (e - c->start) * c->speed);
        if (newSrc < 0) { e -= (Ticks) ((double) newSrc / c->speed); newSrc = 0; }
        if (c->end() - e < minLen) return false;
        shiftKeys (*c, e - c->start);
        c->length = c->end() - e; c->srcIn = newSrc; c->start = e;
    }
    else
    {
        Ticks maxEnd = c->start + (Ticks) ((double) (mediaLen - c->srcIn) / c->speed);
        if (m && m->kind == MediaKind::Image) maxEnd = std::numeric_limits<Ticks>::max() / 4;
        e = std::min (e, maxEnd);
        if (e - c->start < minLen) return false;
        c->length = e - c->start;
    }
    return true;
}

juce::String paintClip (Sequence& s, const juce::String& tid, Ticks at, int track)
{
    auto* t = s.findClip (tid);
    if (t == nullptr || track < 0 || track >= (int) s.tracks.size() || s.tracks[(size_t) track].locked) return {};
    if (s.tracks[(size_t) track].kind != s.tracks[(size_t) t->track].kind) return {};
    Clip c = *t; c.id = newId(); c.start = at; c.track = track;
    clearTrackRange (s, track, at, at + c.length);
    s.clips.push_back (c); s.sortClips();
    return c.id;
}

juce::String addMediaClip (Project& p, const juce::String& mid, int track, Ticks at, Ticks maxLen)
{
    auto* m = p.findMedia (mid);
    auto& s = p.seq();
    if (m == nullptr) return {};
    bool audio = m->kind == MediaKind::Audio;
    if (track < 0 || track >= (int) s.tracks.size() || (s.tracks[(size_t) track].kind == TrackKind::Audio) != audio)
    {
        track = -1;
        for (int i = 0; i < (int) s.tracks.size(); ++i)
            if ((s.tracks[(size_t) i].kind == TrackKind::Audio) == audio) { track = i; break; }
        if (track < 0) track = s.addTrack (audio ? TrackKind::Audio : TrackKind::Video, audio ? "A1" : "V1");
    }
    Clip c; c.id = newId(); c.mediaId = mid; c.track = track; c.start = std::max<Ticks> (0, at);
    c.length = maxLen > 0 ? std::min (m->duration, maxLen) : m->duration; c.volume = audio ? 1.0f : 0.0f;
    clearTrackRange (s, track, c.start, c.end());
    s.clips.push_back (c); s.sortClips();
    return c.id;
}

// ------------------------------------------------------------------ chop / multicam
static int firstVideoTrack (const Sequence& s)
{
    for (int i = 0; i < (int) s.tracks.size(); ++i) if (s.tracks[(size_t) i].kind == TrackKind::Video) return i;
    return 0;
}

static const MediaItem* pickTake (const std::vector<const MediaItem*>& takes, const juce::String& avoid, juce::Random& rng, float qualityWeight)
{
    if (takes.empty()) return nullptr;
    std::vector<double> w;
    double sum = 0;
    for (auto* t : takes)
    {
        double v = (t->id == avoid && takes.size() > 1) ? 0.0 : (1.0 - qualityWeight) + qualityWeight * (0.2 + t->quality) + (t->favorite ? 1.0 : 0.0);
        w.push_back (v); sum += v;
    }
    double r = rng.nextDouble() * sum;
    for (size_t i = 0; i < takes.size(); ++i) { r -= w[i]; if (r <= 0 && w[i] > 0) return takes[i]; }
    for (size_t i = takes.size(); i-- > 0;) if (w[i] > 0) return takes[i];
    return takes[0];
}

static void placeSynced (Sequence& s, int track, const MediaItem& m, Ticks a, Ticks b)
{
    Clip c; c.id = newId(); c.mediaId = m.id; c.track = track; c.start = a; c.length = b - a;
    c.srcIn = a - m.sync.offset;  // keeps lip-sync / performance sync
    c.volume = 0.0f;
    s.clips.push_back (c);
}

int chopRange (Project& p, Ticks a, Ticks b, ChopMode mode, juce::uint32 seed)
{
    auto& s = p.seq();
    if (! p.song.valid || b <= a) return 0;
    auto pts = chopPoints (p.song, a, b, mode);
    pts.insert (pts.begin(), a);
    pts.push_back (b);
    std::sort (pts.begin(), pts.end());
    pts.erase (std::unique (pts.begin(), pts.end()), pts.end());
    int track = firstVideoTrack (s);
    clearTrackRange (s, track, a, b);
    juce::Random rng ((juce::int64) seed);
    juce::String prev;
    int n = 0;
    Ticks minSeg = secToTicks (1.0 / p.fps);
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        Ticks t0 = pts[i], t1 = pts[i + 1];
        if (t1 - t0 < minSeg && i + 2 < pts.size()) { pts[i + 1] = t0; continue; }
        auto takes = takesCovering (p, t0, t1 - t0);
        auto* m = pickTake (takes, prev, rng, 0.3f);
        if (m == nullptr) continue;
        placeSynced (s, track, *m, t0, t1);
        prev = m->id; ++n;
    }
    s.sortClips();
    return n;
}

int layoutSyncedTakes (Project& p)
{
    auto& s = p.seq();
    int n = 0;
    std::vector<const MediaItem*> takes;
    for (auto& m : p.media)
        if (m.kind == MediaKind::Video && (m.sync.status == SyncInfo::Synced || m.sync.status == SyncInfo::Manual)) takes.push_back (&m);
    while (s.videoTrackCount() < (int) takes.size()) s.addTrack (TrackKind::Video, "V" + juce::String (s.videoTrackCount() + 1));
    int vt = 0;
    for (auto* m : takes)
    {
        int track = -1, k = 0;
        for (int i = 0; i < (int) s.tracks.size(); ++i)
            if (s.tracks[(size_t) i].kind == TrackKind::Video && k++ == vt) { track = i; break; }
        Ticks a = std::max<Ticks> (0, m->sync.offset), b = m->sync.offset + m->duration;
        clearTrackRange (s, track, a, b);
        placeSynced (s, track, *m, a, b);
        ++vt; ++n;
    }
    s.sortClips();
    return n;
}

// ------------------------------------------------------------------ generator
struct StyleRules
{
    const char* name;
    const char* look;
    // segment length in beats for section low / mid / high
    double beats[3];
    ChopMode burst;          // used for short bursts in high sections
    double burstChance;      // per bar in high sections
    TransitionType sectionTrans;
    double qualityWeight;
    float beatFx[4];         // kickZoom, snareFlash, shake, hatStrobe
};

static const StyleRules kStyles[3] = {
    { "Cinematic", "Teal Night",        { 8, 4, 2 }, ChopMode::Grid8,  0.0,  TransitionType::Dissolve, 0.8, { 0.0f, 0.0f, 0.0f, 0.0f } },
    { "Underground", "Underground Grit",{ 4, 2, 1 }, ChopMode::HiHat,  0.35, TransitionType::Glitch,   0.3, { 0.0f, 0.25f, 0.15f, 0.25f } },
    { "Performance", "Performance Punch",{ 4, 2, 1 }, ChopMode::Kick,  0.25, TransitionType::Flash,    0.6, { 0.6f, 0.45f, 0.35f, 0.0f } },
};

static std::vector<TransitionType> stylePalette (const juce::String& n)
{
    using T = TransitionType;
    if (n == "Cinematic") return { T::Dissolve, T::DipBlack, T::Blur, T::Flash };
    if (n == "Underground") return { T::Whip, T::Blur, T::Shake, T::Mirror, T::DipBlack };
    return { T::Flash, T::Zoom, T::Whip, T::Shake, T::Spin };
}

static Sequence generateOne (const Project& p, const StyleRules& st, juce::uint32 seed, const juce::String& styleMode)
{
    Sequence s = Project::makeDefaultSequence (juce::String (st.name) + " Cut");
    s.style = st.name; s.seed = seed;
    juce::Random rng ((juce::int64) seed);
    const auto& a = p.song;
    Ticks songLen = std::min (a.duration, secToTicks (kMaxSongSeconds));
    // per-generation variation so every press feels different
    const double paceMul = std::array<double, 4> { 1.0, 0.5, 2.0, 1.0 }[(size_t) rng.nextInt (4)];
    const double burstMul = 0.5 + rng.nextDouble() * 1.2, transMul = 0.6 + rng.nextDouble() * 0.9, punchMul = 0.5 + rng.nextDouble();
    const ChopMode burstModes[] = { st.burst, ChopMode::HiHat, ChopMode::Kick, ChopMode::Grid8, ChopMode::Snare };
    const double flashP = juce::String (st.name) == "Cinematic" ? 0.05 : 0.12;
    int audioTrack = (int) s.tracks.size() - 1;
    if (auto* song = p.song_())
    {
        Clip c; c.id = newId(); c.mediaId = song->id; c.track = audioTrack; c.start = 0; c.length = std::min (song->duration, songLen);
        c.locked = p.musicLock; c.volume = 1.0f;
        s.clips.push_back (c);
    }
    // all takes, synced first
    std::vector<const MediaItem*> broll;
    for (auto& m : p.media)
        if (m.kind == MediaKind::Video && ! m.missing && m.sync.status != SyncInfo::Synced && m.sync.status != SyncInfo::Manual) broll.push_back (&m);

    const double beat = a.beatSec();
    int prevSection = -1;
    std::vector<std::pair<Ticks, Ticks>> flashHits;   // Black Flash 2 is a sequence-level beat effect, so it rides on an effect layer
    juce::String prev;
    // walk the song in musical segments from 0 (pickup before first downbeat is its own segment)
    Ticks t = 0;
    juce::int64 beatIdx = (juce::int64) std::ceil (a.beatAt (0));
    if (a.gridOffset > 0 && a.gridTime ((double) beatIdx) > 0)
    {
        // pickup bar: cut on the first grid line
    }
    while (t < songLen)
    {
        int sec = a.sectionAt (t + 1);
        double segBeats = std::max (1.0, st.beats[juce::jlimit (0, 2, sec)] * paceMul);
        // align segment to the grid
        double bt = a.beatAt (t);
        double nextBeat = std::floor (bt / segBeats + 1e-6) * segBeats + segBeats;
        Ticks t1 = std::min (songLen, a.gridTime (nextBeat));
        if (t1 - t < secToTicks (beat * 0.5)) t1 = std::min (songLen, a.gridTime (nextBeat + segBeats));

        std::vector<Ticks> cuts { t, t1 };
        // active sequencer: hat rolls always cut, hooks burst often, verses sometimes
        const double burstP = (sec == 2 ? std::max (st.burstChance, 0.55) : sec == 1 ? std::max (st.burstChance, 0.2) * 0.8 : st.burstChance * 0.25) * burstMul;
        if (st.burstChance > 0 && rng.nextDouble() < burstP)
            for (auto e : chopPoints (a, t, t1, burstModes[(size_t) rng.nextInt (5)])) cuts.push_back (e);
        if (st.burstChance > 0 || sec == 2)
            for (auto e : hatRolls (a, t, t1)) cuts.push_back (e);
        {
            std::sort (cuts.begin(), cuts.end());
            cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
        }
        for (size_t i = 0; i + 1 < cuts.size(); ++i)
        {
            Ticks c0 = cuts[i], c1 = cuts[i + 1];
            if (c1 - c0 < secToTicks (1.0 / p.fps)) continue;
            auto takes = takesCovering (p, c0, c1 - c0);
            const MediaItem* m = pickTake (takes, prev, rng, (float) st.qualityWeight);
            bool useBroll = m == nullptr || (sec == 0 && ! broll.empty() && st.qualityWeight < 0.9 && rng.nextDouble() < 0.3);
            if (useBroll && ! broll.empty())
            {
                auto* b = broll[(size_t) rng.nextInt ((int) broll.size())];
                Clip c; c.id = newId(); c.mediaId = b->id; c.track = 0; c.start = c0; c.length = c1 - c0; c.volume = 0.0f;
                Ticks room = b->duration - c.length;
                if (room < 0) { c.length = b->duration; }
                c.srcIn = room > 0 ? (Ticks) (rng.nextDouble() * (double) room) : 0;
                if (c.length > 0) s.clips.push_back (c);
                prev = b->id;
            }
            else if (m != nullptr)
            {
                placeSynced (s, 0, *m, c0, c1);
                prev = m->id;
            }
            else continue;
            auto& placed = s.clips.back();
            const double len = ticksToSec (placed.length);
            if (sec != prevSection && prevSection >= 0 && i == 0)
            {
                placed.transIn = st.sectionTrans;
                placed.transLen = secToTicks (std::min (beat, len * 0.5));
            }
            else if (c0 > 0 && rng.nextDouble() < flashP * (sec + 1))
            {   // quick 0.2 s white flash on the cut
                placed.transIn = TransitionType::Flash;
                placed.transLen = std::min (placed.length / 2, secToTicks (0.2));
            }
            else if (c0 > 0 && len >= beat && rng.nextDouble() < 0.04)
            {   // the odd dissolve
                placed.transIn = TransitionType::Dissolve;
                placed.transLen = secToTicks (std::min (beat * 0.5, len * 0.4));
            }
            else if (c0 > 0 && len >= beat * 0.75)
            {
                // transitions on a share of ordinary cuts, short and on-grid
                const double trP = (juce::String (st.name) == "Cinematic" ? 0.12 : 0.25) * (sec + 1) * transMul;
                if (rng.nextDouble() < trP)
                {
                    auto pal = stylePalette (st.name);
                    placed.transIn = pal[(size_t) rng.nextInt ((int) pal.size())];
                    placed.transLen = secToTicks (std::min (beat * 0.5, len * 0.4));
                }
            }
            // punch-in on hits: an eased scale keyframe from 1.1x back to 1x over a quarter beat-ish
            const bool punchy = juce::String (st.name) != "Cinematic";
            if (punchy && sec >= 1 && len >= 0.2 && rng.nextDouble() < (sec == 2 ? 0.45 : 0.2) * punchMul)
            {
                setKey (placed, "scale", 0, 1.08f + 0.08f * (float) rng.nextDouble(), 2);
                setKey (placed, "scale", secToTicks (std::min (0.3, len * 0.6)), 1.0f);
            }
            if (juce::String (st.name) == "Underground" && sec == 2 && len >= beat && rng.nextDouble() < 0.2)
                flashHits.push_back ({ placed.start, placed.length });
        }
        prevSection = sec;
        t = t1;
    }
    if (! flashHits.empty())
    {
        const int ftr = insertVideoTrack (s);
        for (auto& [fa, fl] : flashHits)
        {
            Clip fc; fc.id = newId(); fc.track = ftr; fc.start = fa; fc.length = fl; fc.name = "Black Flash 2";
            fc.fx.set ("blackFlash2", 0.6f);
            s.clips.push_back (fc);
        }
    }
    for (auto& l : lookLibrary()) if (l.name == st.look) applyLook (s.fx, l, 1.0f);
    if (styleMode.isNotEmpty()) applyStyleMode (s.fx, styleMode);
    const char* beatIds[4] = { "kickZoom", "snareFlash", "shake", "hatStrobe" };
    for (int i = 0; i < 4; ++i) if (st.beatFx[i] > 0) s.fx.set (beatIds[i], st.beatFx[i]);
    s.sortClips();
    return s;
}

int randomizeShots (const Project& p, Sequence& s, const juce::StringArray& clipIds, juce::uint32 seed)
{
    juce::Random rng ((juce::int64) seed);
    int n = 0;
    for (auto& id : clipIds)
    {
        auto* c = s.findClip (id);
        if (c == nullptr || c->locked || c->keep || c->speed != 1.0 || c->reverse) continue;
        if (c->track < 0 || c->track >= (int) s.tracks.size() || s.tracks[(size_t) c->track].locked || s.tracks[(size_t) c->track].kind != TrackKind::Video) continue;
        std::vector<const MediaItem*> pool;
        for (auto* m : takesCovering (p, c->start, c->length)) if (m->id != c->mediaId) pool.push_back (m);
        if (pool.empty()) continue;
        auto* m = pool[(size_t) rng.nextInt ((int) pool.size())];
        c->mediaId = m->id; c->srcIn = c->start - m->sync.offset;
        ++n;
    }
    return n;
}

int liftClips (const Project& p, Sequence& s, const juce::StringArray& ids, bool ripple, juce::uint32 seed)
{
    juce::StringArray gone;
    int filled = 0;
    for (auto& id : ids)
    {
        const auto* c = s.findClip (id);
        if (c == nullptr) continue;
        bool editTrack = false;
        for (auto& k : s.clips) editTrack |= k.track == c->track && k.mediaId.isNotEmpty() && k.mediaId != c->mediaId && k.mediaId != p.songId;
        if (! ripple && editTrack && ! c->keep && randomizeShots (p, s, { id }, seed + (juce::uint32) gone.size() * 7919u + (juce::uint32) filled) == 1) { ++filled; continue; }
        gone.add (id);
    }
    deleteClips (s, gone, ripple);
    return filled;
}

int addTakeLayers (const Project& p, Sequence& s)
{
    int n = 0;
    for (auto& m : p.media)
    {
        if (m.kind != MediaKind::Video || m.missing || (m.sync.status != SyncInfo::Synced && m.sync.status != SyncInfo::Manual)) continue;
        const int tk = insertVideoTrack (s);
        auto& tr = s.tracks[(size_t) tk];
        tr.name = m.name; tr.mute = true;   // visible: fills any gap in the edit and shows in the timeline; occluded frames are never decoded
        Clip c; c.id = newId(); c.mediaId = m.id; c.track = tk;
        c.start = std::max ((Ticks) 0, m.sync.offset); c.srcIn = c.start - m.sync.offset;
        c.length = m.duration - c.srcIn; c.volume = 0.0f;
        if (c.length > 0) { s.clips.push_back (c); ++n; }
    }
    s.sortClips();
    return n;
}

bool regenerateActive (Project& p, juce::uint32 seed, const juce::String& styleMode)
{
    if (! p.song.valid) return false;
    auto& cur = p.seq();
    for (auto& rules : kStyles)
        if (cur.style == rules.name)
        {
            auto nm = cur.name;
            auto s = generateOne (p, rules, seed, styleMode);
            s.name = nm;
            cur = s;
            addTakeLayers (p, cur);
            return true;
        }
    return false;
}

std::vector<int> generateEdits (Project& p, juce::uint32 seed, const juce::String& styleMode)
{
    std::vector<int> idx;
    if (! p.song.valid) return idx;
    for (int i = 0; i < 3; ++i)
    {
        p.sequences.push_back (generateOne (p, kStyles[i], seed * 7919u + (juce::uint32) i * 104729u, styleMode));
        addTakeLayers (p, p.sequences.back());
        idx.push_back ((int) p.sequences.size() - 1);
    }
    return idx;
}

int buildSeqEdit (Project& p, juce::uint32 seed)
{
    const auto& a = p.song;
    if (! a.valid) return -1;
    std::vector<const MediaItem*> vids;
    for (auto& m : p.media)
        if (m.kind == MediaKind::Video && ! m.missing && m.id != p.songId && m.duration > 0) vids.push_back (&m);
    if (vids.empty()) return -1;
    int n = 1;
    for (auto& q : p.sequences) if (q.name.startsWith ("SEQ ")) ++n;
    Sequence s = Project::makeDefaultSequence ("SEQ " + juce::String (n));
    s.style = "SEQ"; s.seed = seed;
    juce::Random rng ((juce::int64) seed);
    const Ticks songLen = std::min (a.duration, secToTicks (kMaxSongSeconds));
    if (auto* song = p.song_())
    {
        Clip c; c.id = newId(); c.mediaId = song->id; c.track = (int) s.tracks.size() - 1; c.start = 0; c.length = std::min (song->duration, songLen);
        c.locked = p.musicLock; c.volume = 1.0f;
        s.clips.push_back (c);
    }
    // cut points: a bar in verses, 2 beats in pre-hooks, every beat + hat rolls in hooks/drops
    std::vector<Ticks> cuts { 0 };
    for (Ticks t = 0; t < songLen;)
    {
        const int sec = juce::jlimit (0, 2, a.sectionAt (t + 1));
        const double per = sec == 2 ? 1.0 : sec == 1 ? 2.0 : 4.0;
        const double bt = a.beatAt (t);
        Ticks t1 = a.gridTime (std::floor (bt / per + 1e-6) * per + per);
        if (t1 - t < secToTicks (a.beatSec() * 0.5)) t1 = a.gridTime (std::floor (bt / per + 1e-6) * per + 2 * per);
        t1 = std::min (songLen, std::max (t1, t + secToTicks (1.0 / p.fps)));
        if (sec == 2) for (auto e : hatRolls (a, t, t1)) cuts.push_back (e);
        cuts.push_back (t1);
        t = t1;
    }
    for (auto& c : cuts) c = quantizeToFrame (c, p.fps);
    std::sort (cuts.begin(), cuts.end());
    cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
    // deal the videos like cards: each used evenly, never twice in a row, each one plays forward through its own footage
    std::vector<const MediaItem*> deck;
    std::map<juce::String, Ticks> cursor;
    for (auto* m : vids) cursor[m->id] = (Ticks) (rng.nextDouble() * 0.3 * (double) m->duration);
    juce::String prev;
    // smart pick from each video's moments curve: hooks put the biggest motion hit right on the cut,
    // verses take steady sharp shots; footage already used is avoided
    std::map<juce::String, std::vector<std::pair<Ticks, Ticks>>> used;
    auto pick = [&] (const MediaItem& m, Ticks len, int sec) -> Ticks
    {
        const auto& mo = m.moments; const double hz = 4.0;
        const int n = (int) mo.size(), w = std::max (1, (int) std::ceil (ticksToSec (len) * hz));
        auto& u = used[m.id];
        double best = -1e9; int bi = -1;
        for (int i = 0; i + w <= n; ++i)
        {
            const Ticks s0 = secToTicks (i / hz);
            if (s0 + len > m.duration) break;
            double sc = 0; for (int k = i; k < i + w; ++k) sc += mo[(size_t) k]; sc /= w;
            if (sec == 2) sc = 0.4 * sc + 0.6 * mo[(size_t) i];
            else if (sec == 0) sc = 1.0 - std::abs (sc - 0.45);
            for (auto& [a0, a1] : u) if (s0 < a1 && s0 + len > a0) sc -= 0.6;
            sc += rng.nextDouble() * 0.15;
            if (sc > best) { best = sc; bi = i; }
        }
        if (bi < 0) return -1;
        const Ticks s0 = secToTicks (bi / hz);
        u.push_back ({ s0, s0 + len });
        return s0;
    };
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
    {
        const Ticks c0 = cuts[i], c1 = cuts[i + 1];
        if (c1 - c0 < secToTicks (1.0 / p.fps)) continue;
        if (deck.empty())
        {
            deck = vids;
            for (size_t k = deck.size(); k > 1; --k) std::swap (deck[k - 1], deck[(size_t) rng.nextInt ((int) k)]);
            if (deck.size() > 1 && deck.back()->id == prev) std::swap (deck.front(), deck.back());
        }
        const MediaItem* m = deck.back(); deck.pop_back();
        Clip c; c.id = newId(); c.mediaId = m->id; c.track = 0; c.start = c0; c.length = std::min (c1 - c0, m->duration); c.volume = 0.0f;
        Ticks& cur = cursor[m->id];
        if (const Ticks sp = m->moments.empty() ? -1 : pick (*m, c.length, juce::jlimit (0, 2, a.sectionAt (c0 + 1))); sp >= 0)
            c.srcIn = sp;
        else
        {
            if (cur + c.length > m->duration) cur = 0;
            c.srcIn = cur; cur += c.length;
        }
        s.clips.push_back (c);
        prev = m->id;
    }
    s.sortClips();
    p.sequences.push_back (s);
    return (int) p.sequences.size() - 1;
}

int stackGenerated (Project& p, juce::uint32 seed, const juce::String& styleMode)
{
    if (! p.song.valid) return 0;
    const Sequence g = generateOne (p, kStyles[seed % 3], seed * 7919u + 31337u, styleMode);
    auto& s = p.seq();
    int layer = 1;
    for (auto& t : s.tracks) if (t.name.startsWith ("VX")) ++layer;
    std::map<int, int> tmap;
    int n = 0;
    for (auto& c : g.clips)
    {
        if (c.track < 0 || c.track >= (int) g.tracks.size() || c.mediaId == p.songId || g.tracks[(size_t) c.track].kind != TrackKind::Video) continue;
        if (! tmap.count (c.track))
            tmap[c.track] = s.addTrack (TrackKind::Video, "VX" + juce::String (layer++));
        Clip k = c;
        k.track = tmap[c.track];
        k.id = juce::Uuid().toString();
        k.group = 0;
        s.clips.push_back (k);
        ++n;
    }
    s.sortClips();
    return n;
}
}

namespace isfx
{
juce::StringArray autoCutNames() { return { "1/4", "1/8", "1/16", "1/32", "Triplets", "Hi-hats", "Kicks", "Snares", "Roll (1/8 > 1/16 > 1/32)", "Hi-hat rolls only",
                                                     "Crazy: spray takes on every hat + roll", "Stutter: repeat the shot on hats", "Smooth: bars only",
                                                     "Half-bar (every 2 beats)", "Off-beat (the 'and')", "Build-up (speeds up into the drop)", "Dotted 1/8 (bounce)" }; }

int autoCutClip (Project& p, const juce::String& clipId, AutoCut mode, bool switchAngles, juce::uint32 seed)
{
    auto& s = p.seq();
    auto* cp = s.findClip (clipId);
    if (cp == nullptr || cp->locked || cp->track < 0 || cp->track >= (int) s.tracks.size() || s.tracks[(size_t) cp->track].locked) return 0;
    const Clip orig = *cp;
    const bool video = s.tracks[(size_t) orig.track].kind == TrackKind::Video;
    SongAnalysis g = p.song;
    if (! g.valid) { g = SongAnalysis(); g.bpm = 120; g.gridOffset = 0; }
    const Ticks a = orig.start, b = orig.end();
    auto q = [&] (Ticks t) { return video ? quantizeToFrame (t, p.fps) : t; };
    std::vector<Ticks> pts;
    std::vector<bool> stutter;
    Ticks rollStart = -1;
    auto grid = [&] (int per, Ticks from, Ticks to)
    {
        auto n0 = (juce::int64) std::ceil (g.beatAt (from) * per - 1e-9), n1 = (juce::int64) std::floor (g.beatAt (to) * per + 1e-9);
        for (auto n = n0; n <= n1; ++n) { Ticks t = q (g.subdivTime (n, per)); if (t > a && t < b) pts.push_back (t); }
    };
    switch (mode)
    {
        case AutoCut::Quarter: grid (1, a, b); break;
        case AutoCut::Eighth: grid (2, a, b); break;
        case AutoCut::Sixteenth: grid (4, a, b); break;
        case AutoCut::ThirtySecond: grid (8, a, b); break;
        case AutoCut::Triplet: grid (3, a, b); break;
        case AutoCut::HiHat: case AutoCut::Kick: case AutoCut::Snare:
        {
            auto ty = mode == AutoCut::HiHat ? EventType::HiHat : mode == AutoCut::Kick ? EventType::Kick : EventType::Snare;
            Ticks minGap = secToTicks (video ? 1.0 / p.fps : 0.01);
            for (auto& e : p.song.events)
                if (e.type == ty && e.time > a + minGap && e.time < b - minGap && (pts.empty() || e.time - pts.back() >= minGap)) pts.push_back (q (e.time));
            break;
        }
        case AutoCut::Crazy: case AutoCut::Stutter:
        {
            // every hat, every hat-roll hit, and 1/8 fill where the hats go quiet
            Ticks minGap = secToTicks (video ? 1.0 / p.fps : 0.01);
            std::vector<Ticks> c;
            for (auto& e : p.song.events) if (e.type == EventType::HiHat && e.time > a && e.time < b) c.push_back (e.time);
            for (auto e : hatRolls (p.song, a, b)) c.push_back (e);
            const size_t nHat = c.size();
            grid (2, a, b);
            if (nHat > 0) { c.insert (c.end(), pts.begin(), pts.end()); pts.clear(); }
            else { c = pts; pts.clear(); }
            std::sort (c.begin(), c.end());
            for (auto t : c) if (t > a + minGap && t < b - minGap && (pts.empty() || q (t) - pts.back() >= minGap)) pts.push_back (q (t));
            if (mode == AutoCut::Stutter) rollStart = a;
            break;
        }
        case AutoCut::Smooth:   // bar lines straight from the grid, then frame-quantized
        {
            const int bpb = std::max (1, g.beatsPerBar);
            auto n0 = (juce::int64) std::ceil (g.beatAt (a) / bpb - 1e-9), n1 = (juce::int64) std::floor (g.beatAt (b) / bpb + 1e-9);
            for (auto n = n0; n <= n1; ++n) { Ticks t = q (g.subdivTime (n * bpb, 1)); if (t > a && t < b) pts.push_back (t); }
            break;
        }
        case AutoCut::HalfBar:
            grid (1, a, b);
            pts.erase (std::remove_if (pts.begin(), pts.end(), [&] (Ticks t) { return (juce::int64) std::llround (g.beatAt (t)) % 2 != 0; }), pts.end());
            break;
        case AutoCut::Offbeat:
            grid (2, a, b);
            pts.erase (std::remove_if (pts.begin(), pts.end(), [&] (Ticks t) { return std::llround (g.beatAt (t) * 2) % 2 == 0; }), pts.end());
            break;
        case AutoCut::Dotted:
        {
            const double b0 = g.beatAt (a), b1 = g.beatAt (b);
            for (double x = std::ceil (b0 / 0.75) * 0.75; x < b1; x += 0.75) { Ticks t = q (g.gridTime (x)); if (t > a && t < b) pts.push_back (t); }
            break;
        }
        case AutoCut::Build:   // bars -> beats -> 1/8 -> 1/16 across the clip
        {
            const double b0 = g.beatAt (a), b1 = g.beatAt (b), span = b1 - b0;
            const double steps[] = { (double) std::max (1, g.beatsPerBar), 1.0, 0.5, 0.25 };
            for (int seg = 0; seg < 4; ++seg)
            {
                const double f = b0 + span * seg / 4.0, to = b0 + span * (seg + 1) / 4.0, st = steps[seg];
                for (double x = std::ceil (f / st - 1e-9) * st; x < to - 1e-6; x += st) { Ticks t = q (g.gridTime (x)); if (t > a && t < b) pts.push_back (t); }
            }
            break;
        }
        case AutoCut::HatRoll:
        {
            Ticks minGap = secToTicks (video ? 1.0 / p.fps : 0.01);
            for (auto e : hatRolls (p.song, a, b))
                if (e > a + minGap && e < b - minGap && (pts.empty() || e - pts.back() >= minGap)) pts.push_back (q (e));
            break;
        }
        case AutoCut::Roll:
        {
            // the roll fills the clip's last bar (or the whole clip if shorter): 2 beats of 1/8, 1 of 1/16, 1 of 1/32
            double barB = g.beatsPerBar;
            double endBeat = g.beatAt (b), startBeat = std::max (g.beatAt (a), endBeat - barB);
            double span = endBeat - startBeat;
            rollStart = q (g.gridTime (startBeat));
            if (rollStart > a) pts.push_back (rollStart);
            struct Seg { double from, to; int per; } segs[3] = { { 0, .5, 2 }, { .5, .75, 4 }, { .75, 1, 8 } };
            for (auto& sg : segs)
            {
                double f = startBeat + span * sg.from, t = startBeat + span * sg.to;
                int per = std::max (1, (int) std::lround (sg.per * barB / std::max (0.25, span) * span / barB));
                double step = (t - f) / std::max (1.0, std::round ((t - f) * per));
                for (double x = f; x < t - 1e-6; x += step) { Ticks tt = q (g.gridTime (x)); if (tt > a && tt < b) pts.push_back (tt); }
            }
            break;
        }
    }
    std::sort (pts.begin(), pts.end());
    pts.erase (std::unique (pts.begin(), pts.end()), pts.end());
    if (pts.empty()) return 0;

    s.clips.erase (std::remove_if (s.clips.begin(), s.clips.end(), [&] (const Clip& c) { return c.id == clipId; }), s.clips.end());
    std::vector<Ticks> edges { a }; edges.insert (edges.end(), pts.begin(), pts.end()); edges.push_back (b);
    juce::Random rng ((juce::int64) seed);
    const Ticks rollSrc = rollStart >= 0 ? orig.srcAt (std::max (a, rollStart)) : 0;
    juce::String lastMedia = orig.mediaId;
    int made = 0;
    for (size_t i = 0; i + 1 < edges.size(); ++i)
    {
        Clip k = orig; k.id = newId();
        k.start = edges[i]; k.length = edges[i + 1] - edges[i];
        if (k.length <= 0) continue;
        k.srcIn = orig.srcAt (k.start);
        if (i > 0) { k.transIn = TransitionType::Cut; k.transLen = 0; }
        if ((mode == AutoCut::Roll || mode == AutoCut::Stutter) && rollStart >= 0 && k.start >= rollStart) k.srcIn = rollSrc;   // stutter
        if ((switchAngles || mode == AutoCut::Crazy) && video && i > 0 && mode != AutoCut::Roll && mode != AutoCut::Stutter)
        {
            auto takes = takesCovering (p, k.start, k.length);
            std::vector<const MediaItem*> other;
            for (auto* m : takes) if (m->id != lastMedia) other.push_back (m);
            if (! other.empty())
            {
                auto* m = other[(size_t) rng.nextInt ((int) other.size())];
                k.mediaId = m->id; k.srcIn = k.start - m->sync.offset; k.speed = 1.0;
            }
        }
        lastMedia = k.mediaId;
        s.clips.push_back (k); ++made;
    }
    s.sortClips();
    return made;
}
// ------------------------------------------------------------------ Premiere-style clip commands
std::vector<Clip> copyClips (const Sequence& s, const juce::StringArray& ids)
{
    std::vector<Clip> out;
    for (auto& c : s.clips) if (ids.contains (c.id)) out.push_back (c);
    return out;
}

juce::StringArray pasteClips (Sequence& s, const std::vector<Clip>& clips, Ticks at)
{
    juce::StringArray ids;
    if (clips.empty()) return ids;
    Ticks first = clips.front().start;
    for (auto& c : clips) first = std::min (first, c.start);
    for (auto c : clips)
    {
        if (c.track < 0 || c.track >= (int) s.tracks.size() || s.tracks[(size_t) c.track].locked) continue;
        c.id = newId(); c.start = at + (c.start - first); c.locked = false; c.group = 0;
        s.clips.push_back (c); ids.add (c.id);
    }
    settleClips (s, ids);   // overwrite paste: only covers the pasted span
    return ids;
}

juce::StringArray duplicateClips (Sequence& s, const juce::StringArray& ids)
{
    auto src = copyClips (s, ids);
    if (src.empty()) return {};
    Ticks end = 0;
    for (auto& c : src) end = std::max (end, c.end());
    return pasteClips (s, src, end);
}

void pasteAttributes (Sequence& s, const Clip& from, const juce::StringArray& ids)
{
    for (auto& c : s.clips)
        if (ids.contains (c.id) && ! c.locked)
        {
            c.fx = from.fx; c.opacity = from.opacity; c.volume = from.volume;
            c.transIn = from.transIn; c.transLen = from.transLen;
        }
}

juce::String freezeFrame (Sequence& s, const juce::String& id, Ticks at, Ticks len)
{
    auto* c = s.findClip (id);
    if (c == nullptr || at <= c->start || at >= c->end() || len <= 0) return {};
    const int track = c->track;
    if (! splitAt (s, at, { id })) return {};
    Clip* mid = nullptr;
    for (auto& k : s.clips) if (k.track == track && k.start == at) mid = &k;
    if (mid == nullptr) return {};
    juce::String mid_id = mid->id;
    if (mid->length > len) splitAt (s, at + len, { mid_id });
    mid = s.findClip (mid_id);
    mid->speed = 0.0;   // srcIn already points at the frame under the playhead; the rest of the clip keeps its song position
    return mid_id;
}

bool swapClipSlots (Sequence& s, const Project& p, const juce::String& ida, const juce::String& idb)
{
    auto* a = s.findClip (ida); auto* b = s.findClip (idb);
    if (a == nullptr || b == nullptr || a == b || a->locked || b->locked) return false;
    std::swap (a->track, b->track); std::swap (a->start, b->start); std::swap (a->length, b->length);
    for (auto* c : { a, b })
        if (auto* m = p.findMedia (c->mediaId); m != nullptr && m->kind != MediaKind::Image && m->duration > 0 && ! c->ramped() && c->speed > 0)
        {
            const Ticks need = (Ticks) ((double) c->length * c->speed);
            if (c->srcIn + need > m->duration) c->srcIn = std::max<Ticks> (0, m->duration - need);
            if (c->srcIn + need > m->duration) c->speed = (double) (m->duration - c->srcIn) / (double) std::max<Ticks> (1, c->length);   // short shot: slow it to fill the slot
        }
    s.sortClips();
    return true;
}

juce::StringArray speedRampNames()
{
    return { "Ramp into slow-mo (100% > 25%)", "Slow-mo into fast (25% > 200%)", "Speed dip (100% > 20% > 100%)",
             "Fast in, slow out (300% > 50%)", "Bullet time (150% > 10% > 150%)" };
}

bool applySpeedRamp (Sequence& s, const Project& p, const juce::String& id, int preset)
{
    auto* c = s.findClip (id);
    if (c == nullptr || c->locked || c->mediaId.isEmpty()) return false;
    c->keys.erase ("speed");
    if (preset < 0) return true;
    static const std::vector<std::vector<std::pair<float, float>>> R {
        { { .35f, 1 }, { .6f, .25f } }, { { .4f, .25f }, { .7f, 2 } }, { { .25f, 1 }, { .45f, .2f }, { .55f, .2f }, { .75f, 1 } },
        { { .2f, 3 }, { .5f, .5f } }, { { .2f, 1.5f }, { .4f, .1f }, { .6f, .1f }, { .8f, 1.5f } } };
    if (preset >= (int) R.size()) return false;
    for (auto& [u, v] : R[(size_t) preset]) setKey (*c, "speed", (Ticks) ((double) c->length * u), v, 2);
    if (auto* m = p.findMedia (c->mediaId); m != nullptr && m->kind != MediaKind::Image && m->duration > 0)
    {
        const double avail = (double) (m->duration - c->srcIn);
        if (rampedOffset (*c, c->length) > avail)
        {
            Ticks lo = 1, hi = c->length;
            while (hi - lo > 1) { Ticks mid = (lo + hi) / 2; (rampedOffset (*c, mid) <= avail ? lo : hi) = mid; }
            c->length = lo;
        }
    }
    return true;
}

bool setClipSpeed (Sequence& s, const Project& p, const juce::String& id, double speed)
{
    auto* c = s.findClip (id);
    if (c == nullptr || c->locked || speed <= 0) return false;
    const double was = c->ramped() ? rampedOffset (*c, c->length) / std::max<Ticks> (1, c->length) : (c->speed > 0 ? c->speed : 1.0);
    c->keys.erase ("speed");
    Ticks len = (Ticks) std::llround ((double) c->length * was / speed);
    if (auto* m = p.findMedia (c->mediaId); m != nullptr && m->kind != MediaKind::Image)
        len = std::min (len, (Ticks) ((double) (m->duration - c->srcIn) / speed));
    for (auto& o : s.clips)
        if (o.track == c->track && &o != c && o.start >= c->end()) len = std::min (len, o.start - c->start);
    c->speed = speed; c->length = std::max<Ticks> (1, len);
    if (speed < 1.0 && c->frameInterp == 1) c->frameInterp = 2;   // slow-mo defaults to optical flow (export + paused preview)
    return true;
}
}
