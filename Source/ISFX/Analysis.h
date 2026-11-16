#pragma once
#include "Model.h"

namespace isfx
{
constexpr int kAnalysisRate = 22050;

// Onset-strength envelope (spectral flux) at hopSec resolution, with per-band fluxes.
struct OnsetEnvelope
{
    double hopSec = 512.0 / kAnalysisRate;
    std::vector<float> all, low, mid, high;   // low <150 Hz (kick), mid 150-4k (snare body), high >6k (hats)
    std::vector<float> rms;
};

OnsetEnvelope computeOnsets (const std::vector<float>& mono, int rate);

// Full song analysis: tempo, beat-0 offset/downbeat, kick/snare/hat events, bar energy + sections.
SongAnalysis analyzeSong (const std::vector<float>& mono, int rate);

// Re-derive downbeat/sections after the user overrides BPM or grid offset.
void reapplyGrid (SongAnalysis& a, double bpm, Ticks offset);

struct SyncResult
{
    bool ok = false;
    Ticks offset = 0;       // master-song time at which the take's t=0 plays
    float confidence = 0;   // 0..1 (peak prominence of correlation)
    juce::String note;
};

// Cross-correlates the take's own audio with the master song.
SyncResult syncToSong (const std::vector<float>& song, const std::vector<float>& take, int rate);
// Syncs one piece of a file (e.g. a short clip) by its own sound. Short pieces are matched with at least
// `minContext` seconds of surrounding audio. Returned offset = master-song time of the file's t = 0.
SyncResult syncSegment (const std::vector<float>& song, const std::vector<float>& fileAudio, int rate,
                        Ticks srcIn, Ticks len, double minContext = 3.0);

// Shot quality 0..1 from a few decoded frames (sharpness + exposure); used by the generator.
float scoreShotQuality (const juce::File& video);
// "best moments" curve at 4 Hz: motion energy + sharpness, normalised per video to 0..1 (background thread)
// Hard cuts inside an already-edited video, as source times of the first frame of each new shot (frame accurate).
std::vector<Ticks> detectCuts (const juce::File& video, Ticks from, Ticks to,
                               std::function<bool()> cancel = {}, std::function<void (double)> progress = {});
std::vector<float> scoreMoments (const juce::File& video, std::function<bool()> cancel = {});
}
