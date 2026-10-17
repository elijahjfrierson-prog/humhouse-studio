#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <map>
#include <array>

namespace hhs
{
// ---------------------------------------------------------------- scales
struct ScaleDef { const char* name; std::vector<int> steps; };
const std::vector<ScaleDef>& allScales();
const char* noteName (int midiKey);          // "C#4"
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

enum class ChannelType { Sampler, Synth, Plugin };

struct SynthParams
{
    int   wave = 0;        // 0 saw 1 square 2 triangle 3 sine 4 808
    float attack = 0.002f, decay = 0.3f, sustain = 0.7f, release = 0.25f;
    float cutoff = 12000.0f, resonance = 0.2f;
    float detune = 0.0f;   // cents for 2nd osc
    int   unison = 1;
    float drive = 0.0f;
};

struct Channel
{
    int id = 0;
    juce::String name;
    ChannelType type = ChannelType::Sampler;
    juce::File sampleFile;                 // sampler
    int builtinDrum = -1;                  // synthesized fallback kit voice
    SynthParams synth;                     // synth
    juce::String pluginId;                 // plugin: PluginDescription::createIdentifierString
    juce::MemoryBlock pluginState;
    int   mixerTrack = 0;                  // 0 = master
    float volume = 0.8f, pan = 0.0f, pitch = 0.0f; // pitch in semitones
    float glide = 0.0f;                    // seconds, 0 = off
    bool  mono = false;                    // legato/mono mode
    bool  muted = false, solo = false;
    int   rootKey = 60;                    // step-sequencer note
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

// ---------------------------------------------------------------- playlist
struct Clip
{
    int    uid = 0;
    int    lane = 0;
    double start = 0.0;            // beats
    double length = 4.0;           // beats
    int    patternIndex = -1;      // pattern clip if >=0
    juce::File audioFile;          // audio clip otherwise
    double sourceOffset = 0.0;     // seconds into the audio file
    float  gain = 1.0f;
    bool   muted = false, selected = false, reversed = false;
    int    mixerTrack = -1;         // audio routing follows the recording (FL style); -1 = lane's insert
    double fadeIn = 0.0, fadeOut = 0.0; // beats
    float  fadeCurve = 0.0f;        // -1..1 (0 = linear)
    int    takeGroup = 0;           // loop / punch takes share a group; only the visible take plays
    bool   hidden = false;
    bool isAudio() const { return patternIndex < 0; }
};

struct Lane
{
    juce::String name;
    int  inputPair = 0;            // stereo input pair (0 = in 1/2)
    int  mixerTrack = 0;
    bool armed = false, muted = false;
    bool monitor = true;
    float height = 56.0f;
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
    juce::File projectFile;

    int nextChannelId = 1;
    int nextUid = 1;
    int newUid() { return nextUid++; }
    FxSlot& addFx (int track, const juce::String& type);

    static Project createDefault();
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
}
