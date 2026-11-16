#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <memory>
#include <cmath>

namespace isfx
{
// ---- time: flicks (1/705600000 s) ---------------------------------------------
using Ticks = juce::int64;
constexpr Ticks kFlicks = 705600000LL;
inline Ticks secToTicks (double s)        { return (Ticks) std::llround (s * (double) kFlicks); }
inline double ticksToSec (Ticks t)        { return (double) t / (double) kFlicks; }
inline Ticks frameToTicks (juce::int64 f, double fps) { return secToTicks ((double) f / fps); }
inline juce::int64 ticksToFrame (Ticks t, double fps) { return (juce::int64) std::floor (ticksToSec (t) * fps + 1e-9); }
inline Ticks quantizeToFrame (Ticks t, double fps)    { return secToTicks (std::round (ticksToSec (t) * fps) / fps); }

// ---- musical analysis -----------------------------------------------------------
enum class EventType { Kick = 0, Snare, HiHat, Onset };

struct MusicEvent
{
    Ticks time = 0;
    EventType type = EventType::Onset;
    float strength = 0, confidence = 0;
};

struct SongAnalysis
{
    bool valid = false;
    double bpm = 120.0;
    Ticks gridOffset = 0;           // time of beat 0 (a downbeat)
    int beatsPerBar = 4;
    std::vector<MusicEvent> events; // detected drum events
    std::vector<float> barEnergy;   // 0..1 per bar (RMS, normalized)
    std::vector<int> barSection;    // 0 = low, 1 = mid, 2 = high (chorus/drop)
    Ticks duration = 0;

    double beatSec() const { return 60.0 / bpm; }
    // time of a grid line: beat index + fraction, computed directly (no accumulation)
    Ticks gridTime (double beatIndex) const { return gridOffset + secToTicks (beatIndex * beatSec()); }
    Ticks subdivTime (juce::int64 n, int perBeat) const { return gridOffset + secToTicks ((double) n * beatSec() / perBeat); }
    double beatAt (Ticks t) const { return ticksToSec (t - gridOffset) / beatSec(); }
    int barAt (Ticks t) const { return (int) std::floor (beatAt (t) / beatsPerBar); }
    int sectionAt (Ticks t) const;
    float energyAt (Ticks t) const;
};

// ---- effects ----------------------------------------------------------------------
// A flat parameter map; ids are defined in Looks.h (fx::).  Missing ids = defaults.
struct FxStack
{
    std::map<juce::String, float> p;
    juce::String lutPath;            // optional .cube
    float get (const juce::String& id, float def) const { auto it = p.find (id); return it == p.end() ? def : it->second; }
    void set (const juce::String& id, float v) { p[id] = v; }
};

enum class TransitionType { Cut = 0, Dissolve, DipBlack, DipWhite, Flash, Zoom, Whip, Glitch,
                            Spin, RgbSplit, Blur, LumaWipe, InkBurn, Shake, Stretch, Mirror, Prism, LightLeak, BlackFlash };

// ---- media ------------------------------------------------------------------------
enum class MediaKind { Video = 0, Audio, Image };

struct SyncInfo
{
    enum Status { None = 0, Synced, Uncertain, Failed, Manual };
    Status status = None;
    Ticks offset = 0;          // timeline time of the source's t = 0
    float confidence = 0;
    juce::String note;
};

struct MediaItem
{
    juce::String id, path, name;
    juce::String proxyPath;    // light intra-only copy used for preview playback (export always uses path)
    MediaKind kind = MediaKind::Video;
    Ticks duration = 0;
    double fps = 30.0;
    int width = 0, height = 0, rotation = 0;
    bool hasAudio = false;
    bool favorite = false;
    float quality = 0.5f;      // shot score 0..1 (sharpness/exposure)
    SyncInfo sync;
    bool missing = false;
    std::vector<float> moments;   // 4 per second: how good each moment is (motion + sharpness, 0..1); SEQ picks the best ones
};

// ---- timeline -----------------------------------------------------------------------
struct Clip;
// source ticks consumed from clip start to clip-relative time rel: rel * speed, or the integral of the "speed" keyframes (speed ramp)
double rampedOffset (const Clip& c, Ticks rel);

struct Clip
{
    juce::String id, mediaId;
    int track = 0;
    Ticks start = 0, length = 0, srcIn = 0;
    double speed = 1.0;
    bool locked = false;
    int group = 0;
    float opacity = 1.0f;
    float volume = 1.0f;
    TransitionType transIn = TransitionType::Cut;
    Ticks transLen = 0;
    juce::String name;       // display name override (Rename)
    // keyframes per fx param: (time from clip start, value), sorted; interp 0 = linear, 1 = hold, 2 = ease in/out
    struct Key { Ticks t = 0; float v = 0; int interp = 0; };
    std::map<juce::String, std::vector<Key>> keys;
    bool enabled = true;     // Enable/Disable: a disabled clip stays on the timeline but is not seen or heard
    int label = 0;           // label colour index (0 = default)
    FxStack fx;              // per-clip: exposure, contrast, saturation, scale, posX, posY, rotation, flash
    Ticks end() const { return start + length; }
    bool reverse = false;    // plays the same source range backwards
    bool keep = false;       // pinned: Randomize shot leaves it alone
    Ticks fadeIn = 0, fadeOut = 0;   // audio fades at the clip edges
    juce::String text, font; // text clip (no media): rendered as a layer, so transforms, effects and keyframes apply
    int frameInterp = 1;     // slow motion between source frames: 0 = nearest frame, 1 = frame blending, 2 = optical flow
    bool ramped() const { auto it = keys.find ("speed"); return it != keys.end() && ! it->second.empty(); }
    double srcPos (Ticks t) const
    {
        if (! ramped())
            return reverse ? (double) srcIn + (double) (end() - t) * speed - 1 : (double) srcIn + (double) (t - start) * speed;
        if (reverse) return (double) srcIn + rampedOffset (*this, length) - rampedOffset (*this, t - start) - 1;
        return (double) srcIn + rampedOffset (*this, t - start);
    }
    Ticks srcAt (Ticks t) const { return (Ticks) std::llround (srcPos (t)); }
};

// value of a keyframed param at clip-relative time rel (clamped to first/last key)
float keyValue (const std::vector<Clip::Key>& k, Ticks rel);
// clip gain at rel: volume (or its "volume" keyframe line) times the edge fades
float volumeGain (const Clip& c, Ticks rel);
// writes every keyframed param of c into c.fx at timeline time t
void applyKeyframes (Clip& c, Ticks t);
void setKey (Clip& c, const juce::String& param, Ticks rel, float v, int interp = 0);
// keep keyframes pinned to timeline time when the clip's start moves by delta (split, left trim)
void shiftKeys (Clip& c, Ticks delta);


enum class TrackKind { Video = 0, Audio };

struct Track
{
    juce::String name;
    TrackKind kind = TrackKind::Video;
    bool mute = false, hidden = false, locked = false, solo = false;
    int height = 64;
    juce::String id;         // set when the track first gets plug-ins
    juce::var plugins;       // track plug-in chain (descriptions + state)
};

struct Sequence
{
    juce::String id, name;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    FxStack fx;              // sequence-wide grade / FX (adjustment layer)
    juce::String style;      // generator style that produced it ("" = manual)
    juce::uint32 seed = 1;
    float duckDb = 0.0f;      // < 0: every other clip's sound drops by this while a duckKey (song) clip plays
    juce::String duckKey;

    Ticks duration() const;
    Clip* findClip (const juce::String& id);
    const Clip* findClip (const juce::String& id) const;
    int videoTrackCount() const;
    int addTrack (TrackKind k, const juce::String& name);
    void sortClips();
};

struct ExportSettings
{
    int width = 1920, height = 1080;
    double fps = 30.0;
    int bitrateMbps = 20;
    juce::String codec = "h264";
};

struct Project
{
    static constexpr int kVersion = 1;
    juce::String name = "Untitled";
    std::vector<MediaItem> media;
    std::vector<Sequence> sequences;
    int active = 0;
    juce::String songId;
    juce::var plugins;           // master plug-in chain (descriptions + state), owned by the app's PluginRack
    SongAnalysis song;
    ExportSettings exportSettings;
    double fps = 30.0;          // timeline frame rate
    int aspectW = 16, aspectH = 9;
    bool musicLock = true;

    Sequence& seq() { return sequences[(size_t) juce::jlimit (0, (int) sequences.size() - 1, active)]; }
    const Sequence& seq() const { return sequences[(size_t) juce::jlimit (0, (int) sequences.size() - 1, active)]; }
    MediaItem* findMedia (const juce::String& id);
    const MediaItem* findMedia (const juce::String& id) const;
    const MediaItem* song_() const { return findMedia (songId); }
    static Sequence makeDefaultSequence (const juce::String& name);
    static Project makeNew();
};

juce::String newId();

// ---- persistence -------------------------------------------------------------------
juce::var toVar (const Project&);
bool fromVar (const juce::var&, Project&, juce::String& error);
bool saveProject (const Project&, const juce::File&, juce::String& error);   // atomic
bool loadProject (const juce::File&, Project&, juce::String& error);

// ---- undo (whole-project snapshots; projects are small, media lives on disk) -----------
class History
{
public:
    void push (const Project& p, const juce::String& what);
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    bool undo (Project& p);
    bool redo (Project& p);
    void clear() { undoStack.clear(); redoStack.clear(); }
    juce::String undoName() const { return undoStack.empty() ? juce::String() : undoStack.back().second; }
private:
    std::vector<std::pair<Project, juce::String>> undoStack, redoStack;
};
}
