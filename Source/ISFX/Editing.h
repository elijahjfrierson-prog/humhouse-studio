#pragma once
#include "Model.h"

namespace isfx
{
constexpr double kMaxSongSeconds = 600.0;   // full music videos up to 10 minutes

enum class SnapMode { Off = 0, Bar, Beat, Half, Quarter, Eighth, Sixteenth, ThirtySecond, Triplet, Frame, Sec1, Ms100, Ms10 };
juce::StringArray snapModeNames();
int snapPerBeat (SnapMode m);                 // grid lines per beat (0 = bar/frame/off)
Ticks snapTime (Ticks t, const Project& p, SnapMode m);

enum class ChopMode { Grid8 = 0, Grid16, Grid32, Kick, Snare, HiHat };
juce::StringArray chopModeNames();

// cut times inside [a, b) for a chop mode (grid modes: exact musical times; event modes: detected hits)
std::vector<Ticks> chopPoints (const SongAnalysis& s, Ticks a, Ticks b, ChopMode m);

// takes whose synced footage covers [t, t + len)
std::vector<const MediaItem*> takesCovering (const Project& p, Ticks t, Ticks len);

// ---- edit operations (all on the active sequence; caller records undo) --------------------
bool splitAt (Sequence& s, Ticks t, const juce::StringArray& onlyClips = {});
void deleteClips (Sequence& s, const juce::StringArray& ids, bool ripple);
void deleteRange (Sequence& s, Ticks a, Ticks b, bool ripple, int track = -1);
void keepRange (Sequence& s, Ticks a, Ticks b);   // removes everything outside [a, b)
bool moveClips (Sequence& s, const juce::StringArray& ids, Ticks delta, int trackDelta);
bool trimClip (Sequence& s, const Project& p, const juce::String& id, bool leftEdge, Ticks newEdge);
juce::String paintClip (Sequence& s, const juce::String& templateId, Ticks at, int track);
juce::String addMediaClip (Project& p, const juce::String& mediaId, int track, Ticks at, Ticks maxLen = 0);
int insertVideoTrack (Sequence&);
// effect layer: a clip with no media whose FX apply to everything under it while it plays (trim/move like any clip)
juce::String addEffectClip (Sequence&, const FxStack& fx, const juce::String& name, Ticks at, Ticks len);
// drops a transition on the cut nearest `at` on a track (it belongs to the incoming clip, so it stays locked to that cut)
bool setTransitionAtCut (Sequence&, int track, Ticks at, TransitionType, Ticks len, Ticks maxDist);
// moves the song; with `withSynced` every synced clip, the beat grid and drum events move with it (sync stays intact)
bool shiftSong (Project&, Ticks delta, bool withSynced);                                    // new top video layer, returns its index
void settleClips (Sequence&, const juce::StringArray& ids);          // dropped clips cover what is under them
void fillGaps (Sequence&, const Project&, int track);                // sync-safe magnet: extend neighbours, never slide
void clearTrackRange (Sequence& s, int track, Ticks a, Ticks b);
std::vector<Clip> copyClips (const Sequence&, const juce::StringArray& ids);
juce::StringArray pasteClips (Sequence&, const std::vector<Clip>&, Ticks at);       // overwrite paste, earliest clip lands at `at`
juce::StringArray duplicateClips (Sequence&, const juce::StringArray& ids);         // copies placed right after the selection
void pasteAttributes (Sequence&, const Clip& from, const juce::StringArray& ids);   // effects, opacity, volume, transition
juce::String freezeFrame (Sequence&, const juce::String& id, Ticks at, Ticks len);  // frame hold segment, returns its id
bool setClipSpeed (Sequence&, const Project&, const juce::String& id, double speed); // Speed/Duration: keeps the start, stops at the next clip   // overwrite helper

// sync-preserving angle switch: replaces V1 content in [a,b) with cuts between synced takes
int chopRange (Project& p, Ticks a, Ticks b, ChopMode m, juce::uint32 seed);

// put every synced take on its own track at its sync position (multicam stack)
int layoutSyncedTakes (Project& p);

// GENERATE 3 EDITS: appends Cinematic / Underground / Performance sequences, returns their indices
// Swap each clip (not locked/kept, normal speed) to a different synced take at the same song time. Returns swaps.
int randomizeShots (const Project& p, Sequence& s, const juce::StringArray& clipIds, juce::uint32 seed);
// Pull clips out of the edit. ripple = Extract (close the gap). Lift keeps the gap, but a synced shot on an edit track
// (a track that mixes takes, like a generated edit) is swapped to another take covering the same span so it never goes black.
// Returns how many gaps were filled from takes.
int liftClips (const Project& p, Sequence& s, const juce::StringArray& ids, bool ripple, juce::uint32 seed);
// Every synced take as one full clip on its own hidden, muted video track (raw material under the edit).
int addTakeLayers (const Project& p, Sequence& s);
std::vector<int> generateEdits (Project& p, juce::uint32 seed, const juce::String& styleMode = {});
// Right-click Auto Cut: slices one clip on the grid / detected hits, or a hi-hat-style roll
// (1/8 -> 1/16 -> 1/32 into the next bar, every slice repeating the roll's first slice = stutter).
// switchAngles: video slices cycle through the other synced takes covering each slice.
enum class AutoCut { Quarter = 0, Eighth, Sixteenth, ThirtySecond, Triplet, HiHat, Kick, Snare, Roll };
juce::StringArray autoCutNames();
int autoCutClip (Project& p, const juce::String& clipId, AutoCut mode, bool switchAngles, juce::uint32 seed = 1);
}
