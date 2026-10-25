#pragma once
#include "Model.h"
#include "Effects.h"
#include <memory>

namespace hhs
{
using SharedSample = std::shared_ptr<const juce::AudioBuffer<float>>;

// Real-time instrument for one channel. All methods are audio-thread safe, no allocation.
class ChannelInstrument
{
public:
    virtual ~ChannelInstrument() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void noteOn (int key, float velocity, bool slide) = 0;
    // start a note that began secondsIn ago (playback started mid-note); samplers jump into the sample
    virtual void noteOnAt (int key, float velocity, bool slide, double secondsIn) { juce::ignoreUnused (secondsIn); noteOn (key, velocity, slide); }
    virtual void noteOff (int key) = 0;
    virtual void allNotesOff() = 0;
    virtual void render (juce::AudioBuffer<float>& out, int start, int num) = 0; // adds into out
    virtual bool isActive() const = 0;
    virtual void update (const Channel&) = 0;
    virtual void choke() { allNotesOff(); }   // fast declicked stop (cut groups)   // pull params from model (message thread, under engine lock)
};

std::unique_ptr<ChannelInstrument> createInstrument (const Channel&, SharedSample sample, double sampleRate);
SharedSample loadSampleFile (const juce::File&, double targetRate);
void sincResample (const float* in, int inLen, float* out, int outLen, double ratio);   // ratio = input samples per output sample
// applies the channel's SMP-page edits (trim, reverse, polarity, boost, normalize, stretch, fades, declick)
SharedSample processSample (SharedSample, const Channel&, double sampleRate, double songBpm = 0.0);
juce::AudioBuffer<float> olaStretch (const juce::AudioBuffer<float>& in, double factor);   // pitch kept, length x factor
// the sample's own tempo: typed in, or from the file name ("Loop 140 BPM", "140bpm"); 0 if unknown
double sampleTempo (const Channel&);

// HumWave wavetables: every frame band-limited (64 harmonics), DC-free and normalised
const juce::StringArray& humWaveFactoryNames();
const juce::StringArray& humWarpNames();
// Serum-style warp: remaps the read phase (or the sample) of one wavetable cycle. read (phase 0..1) -> sample
template <typename Read>
inline float humWarpRead (int m, float a, float ph, float fmRatio, Read&& read)
{
    if (m <= 0 || a <= 0.0f) return read (ph);
    constexpr float tp = juce::MathConstants<float>::twoPi;
    auto wrap = [] (float x) { return x - std::floor (x); };
    switch (m)
    {
        case 1: return read (wrap (ph * (1.0f + a * 7.0f)));                                     // Sync
        case 2: case 3:                                                                          // Bend + / -
        {
            const float x = 2.0f * ph - 1.0f, e = m == 2 ? 1.0f / (1.0f + 3.0f * a) : 1.0f + 3.0f * a;
            return read (0.5f * (1.0f + std::copysign (std::pow (std::abs (x), e), x)));
        }
        case 4: { const float w = 1.0f - 0.95f * a; return ph < w ? read (ph / w) : 0.0f; }     // PWM
        case 5: { const float c = 0.5f - 0.45f * a;                                              // Asym
                  return read (ph < c ? 0.5f * ph / c : 0.5f + 0.5f * (ph - c) / (1.0f - c)); }
        case 6: { const float y = read (ph); return y + a * (read (ph < 0.5f ? ph : 1.0f - ph) - y); }   // Mirror
        case 7: { const float y = read (ph); return ph >= 1.0f - 0.5f * a ? -y : y; }           // Flip
        case 8: { const float lv = std::round (2.0f + (1.0f - a) * (1.0f - a) * 62.0f); return std::round (read (ph) * lv) / lv; }   // Quantize
        case 9: return read (wrap (ph + a * 0.35f * std::sin (tp * ph * juce::jmax (1.0f, std::round (fmRatio)))));   // FM (self, synced)
        case 10: return read (wrap (ph + a * 0.15f * std::sin (tp * ph)));                       // Remap
        case 11: return std::sin (read (ph) * (1.0f + a * 6.0f) * juce::MathConstants<float>::halfPi);   // Fold
        default: { const float st = 6.0f + (1.0f - a) * (1.0f - a) * 250.0f; return read (std::floor (ph * st) / st); }   // Downsample
    }
}
std::shared_ptr<const std::vector<float>> humWaveFactory (int kind);
std::shared_ptr<const std::vector<float>> humWaveFromRaw (std::vector<float> frames);          // frames x humWaveFrameSize raw values
std::shared_ptr<const std::vector<float>> humWaveFromAudio (const juce::AudioBuffer<float>&, int frames = 32);   // a one-shot sliced into frames
}
