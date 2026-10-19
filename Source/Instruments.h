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
SharedSample processSample (SharedSample, const Channel&, double sampleRate);
}
