#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <map>
#include <array>

namespace hhs
{
// drum one-shots keep their pitch on every key; everything else follows the Piano Roll (root key = the WAV as-is)
inline bool isDrumSampleName (juce::String n)
{
    n = n.toLowerCase();
    for (auto* d : { "kick", "kik", "kck", "snare", "snr", "clap", "hat", "hihat", "perc", "rim", "crash", "ride", "cymbal", "shaker", "tamb", "cowbell", "snap", "conga", "bongo" })
        if (n.contains (d)) return true;
    return false;
}

// ---------------------------------------------------------------- scales
struct ScaleDef { const char* name; std::vector<int> steps; };
const std::vector<ScaleDef>& allScales();
const char* noteName (int midiKey);          // FL naming: 60 = "C5"
const char* pitchClassName (int pc);         // "C#"
bool inScale (int midiKey, int root, int scaleIndex);
int  snapToScale (int midiKey, int root, int scaleIndex); // nearest in-scale key

// ---------------------------------------------------------------- notes
struct Note
{
    int    key = 60;
    double start = 0.0;    // beats
    double length = 0.25;  // beats
    float  velocity = 0.8f;
    float  pan = 0.0f;
    bool   muted = false;
    bool   selected = false;
    bool   slide = false;  // glide into this note
    double end() const { return start + length; }
};
using NoteList = std::vector<Note>;

enum class ChannelType { Sampler, Synth, Plugin, Drummer };

struct SynthParams
{
    int   wave = 0;        // 0 saw 1 square 2 triangle 3 sine 4 808 5 supersaw 6 FM 7 morph 8 air/noise
    float attack = 0.002f, decay = 0.3f, sustain = 0.7f, release = 0.25f;
    float cutoff = 12000.0f, resonance = 0.2f;
    float detune = 0.0f;   // cents for 2nd osc
    int   unison = 1;
    float drive = 0.0f;
    float fmRatio = 1.0f, fmIndex = 0.0f;   // FM keys / bells
    float morph = 0.0f;                     // morph wave: sine > triangle > saw > square
    float noise = 0.0f;                     // breath / air mixed into any wave
    float lfoRate = 0.0f, vibrato = 0.0f;   // Hz, semitones (also sweeps the morph)
};

// Session Drummer (Logic-style): how the drummer plays, and how the kit is mixed
struct DrummerParams
{
    float complexity = 0.45f, loudness = 0.55f, intensity = 0.6f, fills = 0.4f;
    float swing = 0.0f, humanize = 0.5f, ghost = 0.5f, hatOpen = 0.0f;
    bool  halfTime = false, ride = false;
    int   character = 1, seed = 1;
    int   kickPattern = 0, snarePattern = 0, hatPattern = 0, tomPattern = 0, crashPattern = 0;   // 0 = Auto (the recorded take)
    int   voicing = 1, space = 1, glow = 0;   // mix voicing (Raw, Modern, Punch, Room, Vintage), room space, squeeze glow
    float punch = 0.55f, glue = 0.45f, drive = 0.2f, micBlend = 0.45f, roomMix = 0.22f, squeeze = 0.0f;
    // per drum group (Kick, Snare, Hats, Toms, Cymbals): level dB, pan, tune semitones, decay 0..1 (1 = shortest)
    std::array<float, 17> partDb {};   // per kit piece (kitPieceOf), on top of the group level
    std::array<float, 5> pieceDb { 0, 0, 0, 0, 0 }, piecePan { 0, 0, 0, 0, 0 }, pieceTune { 0, 0, 0, 0, 0 }, pieceDamp { 0, 0, 0, 0, 0 };
    std::vector<int> sectionVel;              // per 8-bar section: dynamics % (0 / missing = 100), ramps into the next section
    std::vector<int> sectionMute;             // per 8-bar section of the song: bit g set = drum group g silent there
    int groupMask = 31;                       // bit g = this channel plays drum group g (Split by drum)
    std::map<int, std::array<int, 5>> steps;  // manual 8-bar sections: per drum group a 16-step bar (bit = hit), repeated
    juce::String performanceKey() const;      // only what changes the notes played
};

struct Channel
{
    int id = 0;
    juce::String name;
    ChannelType type = ChannelType::Sampler;
    juce::File sampleFile;                 // sampler
    int builtinDrum = -1;                  // synthesized fallback kit voice
    SynthParams synth;                     // synth
    DrummerParams drum;                    // drummer (sampleFile = the kit folder)
    bool midiLink = false;                 // Link MIDI: the keyboard plays this channel (none linked = the selected one)
    int drumLink = 0;                      // Split by drum: plays that Drummer channel's regions (0 = its own)
    juce::String pluginId;                 // plugin: PluginDescription::createIdentifierString
    juce::MemoryBlock pluginState;
    int   mixerTrack = 0;                  // 0 = master
    float volume = 0.8f, pan = 0.0f, pitch = 0.0f; // pitch in semitones
    float glide = 0.0f;                    // seconds, 0 = off
    float shift = 0.0f;                    // ms, FL channel Shift: plays every hit this much late (+) or early (-), off the grid
    bool  mono = false;                    // legato/mono mode
    bool  muted = false, solo = false;
    int   rootKey = 60;                    // step-sequencer note
    bool  keyTrack = true;                 // sampler: notes change pitch; off = every note plays the sample as-is (drums, FX)
    // sampler playback (FL style): one-shot ignores note length; cut itself chokes the previous hit (808s never overlap);
    // channels sharing a cut group choke each other (closed hat cuts open hat)
    bool  oneShot = true, cutItself = false;
    int   cutGroup = 0;
    float ampAttack = 0.0f, ampHold = 0.0f, ampRelease = 0.01f;   // seconds; hold 0 = play to the end
    // FL-style sample settings (SMP page): non-destructive, baked when the sample loads
    bool  smpNormalize = false, smpReverse = false, smpInvert = false, smpDeclick = false, smpTrim = false;   // defaults: the sample plays exactly like the WAV
    float smpBoostDb = 0.0f, smpFadeIn = 0.0f, smpFadeOut = 0.0f;   // fades in seconds
    int   stretchMode = 0;                 // 0 Resample (tape: pitch + speed), 1 Stretch (keep pitch), 2 Stretch X (fine time, pitch without length)
    float stretchTime = 1.0f;              // time multiplier for Stretch / Stretch X
    bool  tempoSync = true;                // loops / rhythmic tails follow the song tempo (keeps pitch)
    float sampleBpm = 0.0f;                // the sample's own tempo; 0 = read it from the file name
    juce::Colour colour { 0xffff8a3d };
};

struct Pattern
{
    juce::String name;
    juce::Colour colour { 0xff3d8aff };
    std::map<int, NoteList> notes;         // by channel id
    double lengthBeats() const;            // rounded up to whole bars (min 1 bar)
    bool   stepOn (int channelId, int step, double stepLen = 0.25) const;
    void   toggleStep (int channelId, int step, int rootKey, double stepLen = 0.25);
};

// ---------------------------------------------------------------- automation (FL-style automation clips)
enum AutoKind { AutoNone = 0, AutoFxParam, AutoInsertVol, AutoInsertPan, AutoChanVol, AutoChanPan, AutoChanPitch };
struct AutoTarget
{
    int kind = AutoNone;
    int track = 0, fxUid = 0, channel = 0;   // mixer insert + FX slot uid, or channel id
    juce::String param;                      // built-in param id, or "#n" = plugin parameter n
    juce::String name;                       // label, e.g. "Room Verb - Mix"
};
struct AutoPoint { double beat = 0.0; float value = 0.5f; };   // beat into the clip's content, value 0..1
float autoValueAt (const std::vector<AutoPoint>& pts, double beat);
float autoToValue (int kind, float v);      // 0..1 -> channel / insert value (FX params map on their own range)
float autoFromValue (int kind, float x);

// ---------------------------------------------------------------- playlist
struct Clip
{
    int    uid = 0;
    int    lane = 0;
    double start = 0.0;            // beats
    double length = 4.0;           // beats
    int    patternIndex = -1;      // pattern clip if >=0
    double patOffset = 0.0;        // pattern clip: beats into the pattern where it starts (slice / left trim)
    juce::File audioFile;          // audio clip otherwise
    double sourceOffset = 0.0;     // seconds into the audio file
    float  gain = 1.0f;
    bool   muted = false, selected = false, reversed = false;
    int    mixerTrack = -1;         // audio routing follows the recording (FL style); -1 = lane's insert
    double fadeIn = 0.0, fadeOut = 0.0; // beats
    float  fadeCurve = 0.0f;        // -1..1 (0 = linear)
    int    takeGroup = 0;           // loop / punch takes share a group; only the visible take plays
    bool   hidden = false;
    int    group = 0;               // FL-style clip group: grouped clips select / move together
    juce::Colour colour {};         // own clip colour (transparent = track colour)
    int    drummer = -1;            // Drummer region: the drummer channel id it plays
    AutoTarget autoTarget;          // automation clip if autoTarget.kind > 0 (patOffset = beats into the envelope)
    std::vector<AutoPoint> autoPts;
    bool isAuto() const { return autoTarget.kind > AutoNone; }
    bool isAudio() const { return patternIndex < 0 && drummer < 0 && ! isAuto(); }
    bool isDrummer() const { return drummer >= 0; }
};

struct Lane
{
    juce::String name;
    int  inputPair = 0;            // stereo input pair (0 = in 1/2)
    int  inputCh = 0;              // mono input channel (0 = in 1)
    bool stereoIn = false;         // false: mono input centred in both ears
    float shiftMs = 0.0f;          // Soft: shift the whole track later (+) / earlier (-) in ms to line vocals up
    bool autoNorm = false;         // zero-latency leveler: brings the voice to a steady strong level
    int  mixerTrack = 0;
    bool armed = false, muted = false;
    bool monitor = true;
    float height = 56.0f;
    juce::Colour colour {};        // track colour for its clips (transparent = pattern / audio defaults)
    juce::Colour nameColour {};    // track name color (transparent = automatic, opposite of what it sits on)
    juce::String nameFont;         // track name font: "" app font, Bold, Italic, Mono, Serif, Sans
};

// ---------------------------------------------------------------- mixer
struct FxSlot
{
    int uid = 0;
    juce::String type;             // built-in id ("eq","comp",...) or "plugin"
    juce::String pluginId;
    juce::MemoryBlock pluginState;
    std::map<juce::String, float> params;
    bool bypass = false;
    float mix = 1.0f;
};

struct Send { int target = 0; float level = 1.0f; };

struct MixerTrack
{
    juce::String name;
    float volume = 0.8f, pan = 0.0f;
    bool  muted = false, solo = false, frozen = false;
    juce::File frozenFile;
    int   inputPair = -1;          // recording input for this insert (-1 none)
    std::vector<FxSlot> fx;
    std::vector<Send> sends;       // master is implicit if sends empty and track != 0
    juce::Colour colour { 0xff2a2f36 };
    bool routesToMaster = true;
    // sidechain duck (one knob): dips this insert under the SC source track
    float duck = 0.0f;             // 0 = off .. 1 = full
    int   duckSource = -1;         // -1 = auto (first armed vocal track)
    float duckThreshold = -40.0f, duckRatio = 4.0f, duckAttack = 5.0f, duckRelease = 150.0f;
};

// ---------------------------------------------------------------- project
struct Project
{
    juce::String title = "Untitled";
    double bpm = 140.0;
    int    beatsPerBar = 4;
    int    keyRoot = 0;            // C
    int    scaleIndex = 1;         // natural minor
    bool   highlightScale = true;
    bool   songMode = false;
    int    currentPattern = 0;
    double snap = 0.25;            // beats; 0 = no snap
    double swing = 0.0;            // 0..1
    double punchIn = -1.0, punchOut = -1.0; // beats, punchOut > punchIn = active time selection
    bool   loopRecord = true;
    bool   returnToStart = true;
    double preRollBars = 1.0;
    bool hasPunch() const { return punchOut > punchIn && punchIn >= 0.0; }
    int  clipInsert (const Clip& c) const;

    std::vector<Channel>    channels;
    std::vector<Pattern>    patterns;
    std::vector<Lane>       lanes;
    std::vector<Clip>       clips;
    std::vector<MixerTrack> mixer;   // [0] master
    std::vector<juce::File> audioPool;   // every audio clip ever used, kept until "Clear unused / Clear all"
    int  insertPatternAfter (int after, Pattern pt);                 // returns the new index; clip indices are remapped
    void removePatterns (const std::function<bool (int)>& drop);    // drops patterns + their clips, remaps the rest
    juce::File projectFile;

    int nextChannelId = 1;
    int nextUid = 1;
    int newUid() { return nextUid++; }
    FxSlot& addFx (int track, const juce::String& type);

    static Project createDefault();   // clean rack: no channels until the user picks samples
    static Project createDemo();      // starter beat (drums, 808, keys)
    Channel* findChannel (int id);
    const Channel* findChannel (int id) const;
    int  addChannel (Channel c);       // returns id, creates mixer track if mixerTrack<0
    int  addMixerTrack (const juce::String& name);
    double songLengthBeats() const;
    juce::File recordingsFolder() const;

    // routing
    std::vector<int> outputsOf (int track) const;          // where a track feeds
    bool wouldCreateCycle (int from, int to) const;
    bool toggleRoute (int from, int to);                   // FL click-to-route
    std::vector<int> processingOrder() const;              // topological, master last
    std::vector<std::vector<int>> processingLevels() const;// parallelisable groups
};

double beatsToSeconds (double beats, double bpm);
double secondsToBeats (double secs, double bpm);
// hi-hat style roll on audio clips: inside [rangeA, rangeB) (whole clip if no range) the hit at the
// range start repeats every `div` beats; audio outside the range is kept. Returns clips rolled.
int rollAudioClips (Project& p, const std::vector<int>& uids, double div, double rangeA = -1, double rangeB = -1);
}
