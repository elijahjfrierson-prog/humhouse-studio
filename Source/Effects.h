#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <memory>
#include <vector>

namespace hhs
{
struct PlayContext
{
    double bpm = 140.0;
    double beatPos = 0.0;   // beat at the start of this block
    bool   playing = false;
    int    keyRoot = 0, scaleIndex = 1;
};

struct ParamDef
{
    const char* id;
    const char* name;
    float min, max, def;
    std::vector<const char*> choices {}; // non-empty = stepped choice
};

class BuiltinEffect
{
public:
    static constexpr int maxParams = 24;
    virtual ~BuiltinEffect() = default;
    virtual const char* typeId() const = 0;
    virtual const std::vector<ParamDef>& params() const = 0;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() {}
    virtual void process (juce::AudioBuffer<float>&, int numSamples, const PlayContext&) = 0;
    virtual double tailSeconds() const { return 0.0; }
    virtual int latencySamples() const { return 0; }

    int   paramIndex (const juce::String& id) const;
    void  setParam (const juce::String& id, float v);
    float getParam (const juce::String& id) const;
    float p (int i) const { return values[(size_t) i].load (std::memory_order_relaxed); }
    void  initDefaults();

protected:
    std::array<std::atomic<float>, maxParams> values {};
};

struct EffectInfo { const char* id; const char* name; const char* category; };
const std::vector<EffectInfo>& builtinEffects();
std::unique_ptr<BuiltinEffect> createBuiltinEffect (const juce::String& typeId);

// biquad used by EQ / de-esser / synth
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] {}, z2[2] {};
    enum Type { LowPass, HighPass, Peak, LowShelf, HighShelf, BandPass };
    void set (Type t, double sr, double freq, double q, double gainDb);
    inline float tick (float x, int ch)
    {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};

// YIN pitch detector + granular shifter, shared by AutoPitch
class PitchShifter
{
public:
    void prepare (double sr);
    void setRatio (float r) { ratio = r; }
    float process (float x);
private:
    std::vector<float> buf;
    int write = 0, size = 0;
    double phase = 0.0;
    float ratio = 1.0f, grain = 1.0f;
};

float detectPitchYin (const float* x, int n, double sr, float threshold = 0.15f); // Hz or 0
}
