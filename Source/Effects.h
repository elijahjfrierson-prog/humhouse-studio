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
    static constexpr int maxParams = 128;
    virtual ~BuiltinEffect() = default;
    virtual const char* typeId() const = 0;
    virtual const std::vector<ParamDef>& params() const = 0;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() {}
    virtual void process (juce::AudioBuffer<float>&, int numSamples, const PlayContext&) = 0;
    virtual double tailSeconds() const { return 0.0; }
    virtual int latencySamples() const { return 0; }
    virtual float meter (int) const { return 0.0f; }          // GR / dynamic band offset in dB, for editor visuals
    virtual bool  hasResponse() const { return false; }
    virtual float responseDb (double) { return 0.0f; }        // EQ curve at hz (after prepare + params)

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

// lock-free audio tap for the Scope (written on the audio thread, read by the UI)
struct ScopeTap
{
    static constexpr int size = 16384;
    std::array<float, size> buf {};
    std::atomic<int> pos { 0 };
    std::atomic<double> rate { 44100.0 };
    void push (const float* l, const float* r, int n)
    {
        int w = pos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) { buf[(size_t) w] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i]; w = (w + 1) & (size - 1); }
        pos.store (w, std::memory_order_release);
    }
    void read (float* dst, int n, int end) const
    {
        n = std::min (n, size);
        for (int i = 0; i < n; ++i) dst[i] = buf[(size_t) ((end - n + i + size) & (size - 1))];
    }
};
ScopeTap& masterScope();

// "Scope" insert: passes audio through untouched and feeds its own tap
class ScopeFx : public BuiltinEffect
{
public:
    ScopeFx() { initDefaults(); }
    const char* typeId() const override { return "scope"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "zoom", "Display zoom", 0.5f, 4.0f, 1.0f } }; return ps; }
    void prepare (double sr, int) override { tap->rate = sr; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    { tap->push (b.getReadPointer (0), b.getNumChannels() > 1 ? b.getReadPointer (1) : nullptr, n); }
    std::shared_ptr<ScopeTap> tap = std::make_shared<ScopeTap>();
};

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
// Auto-Tune style shifter: one resampling read head (full sound, no phasing), re-spliced by whole
// pitch periods at a waveform-matched point with a short crossfade, so it doesn't click. Latency ~0.1-6 ms.
class SpliceShifter
{
public:
    void prepare (double sr);
    void process (float& l, float& r, double ratio, double period);   // period in samples (0 = unknown)
    double delaySamples() const { return d; }
private:
    float read (const std::vector<float>& b, double delay) const;
    double findSplice (double target, int span, int len) const;
    std::vector<float> bufL, bufR;
    int w = 0, mask = 0, xfLen = 0, xfPos = 0;
    double sr = 44100, d = 64, d2 = 64;
};

// PSOLA shifter: pitch-synchronous grains (2 periods, Hann) taken from the input and laid out at the new
// period, never resampled, so the voice keeps its own tone (formants) at any ratio - no chipmunk
class PsolaShifter
{
public:
    void prepare (double sr);
    void process (float& l, float& r, double ratio, double period);   // l/r become the shifted signal
    float dryL = 0, dryR = 0;                                         // input delayed to line up with the output
    double delaySamples() const { return lat; }
private:
    float read (const std::vector<float>& b, double pos) const;
    std::vector<float> inL, inR, outL, outR, wsum;
    int mask = 0;
    long long n = 0;
    double sr = 44100, ts = 0, mark = 0, lat = 0;
};

float detectPitchYin (const float* x, int n, double sr, float threshold = 0.15f, float fallback = 0.0f, float loHz = 70.0f, float hiHz = 1100.0f, bool* rough = nullptr); // Hz or 0; fallback > 0 also accepts rougher (breathy) pitch
}
