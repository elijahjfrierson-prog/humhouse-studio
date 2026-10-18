#include "Effects.h"
#include "Model.h"
#include <cmath>

namespace hhs
{
using P = ParamDef;
static constexpr float kPi = juce::MathConstants<float>::pi;
static inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
static inline float gainToDb (float g)  { return 20.0f * std::log10 (std::max (g, 1.0e-6f)); }

// ------------------------------------------------------------ base
int BuiltinEffect::paramIndex (const juce::String& id) const
{
    auto& ps = params();
    for (size_t i = 0; i < ps.size(); ++i) if (id == ps[i].id) return (int) i;
    return -1;
}
void BuiltinEffect::setParam (const juce::String& id, float v)
{
    const int i = paramIndex (id);
    if (i >= 0) { auto& d = params()[(size_t) i]; values[(size_t) i].store (juce::jlimit (d.min, d.max, v)); }
}
float BuiltinEffect::getParam (const juce::String& id) const
{
    const int i = paramIndex (id);
    return i >= 0 ? p (i) : 0.0f;
}
void BuiltinEffect::initDefaults()
{
    auto& ps = params();
    for (size_t i = 0; i < ps.size() && i < (size_t) maxParams; ++i) values[i].store (ps[i].def);
}

void Biquad::set (Type t, double sr, double f, double q, double gainDb)
{
    f = juce::jlimit (10.0, sr * 0.49, f);
    const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * f / sr;
    const double cw = std::cos (w), sw = std::sin (w), alpha = sw / (2.0 * std::max (0.05, q));
    double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
    switch (t)
    {
        case LowPass:  B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        case HighPass: B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        case BandPass: B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        case Peak:     B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A; break;
        case LowShelf:
        { const double s = 2 * std::sqrt (A) * alpha;
          B0 = A * ((A + 1) - (A - 1) * cw + s); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - s);
          A0 = (A + 1) + (A - 1) * cw + s; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - s; break; }
        case HighShelf:
        { const double s = 2 * std::sqrt (A) * alpha;
          B0 = A * ((A + 1) + (A - 1) * cw + s); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - s);
          A0 = (A + 1) - (A - 1) * cw + s; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - s; break; }
    }
    b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0); a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
}

// ------------------------------------------------------------ Para EQ
class ParaEQ : public BuiltinEffect
{
public:
    ParaEQ() { initDefaults(); }
    const char* typeId() const override { return "eq"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "hpf", "Low Cut", 10, 1000, 20 }, { "lowGain", "Low", -18, 18, 0 }, { "lowFreq", "Low Freq", 30, 500, 100 },
            { "midGain", "Mid", -18, 18, 0 }, { "midFreq", "Mid Freq", 200, 8000, 1000 }, { "midQ", "Mid Q", 0.2f, 8, 0.9f },
            { "highGain", "High", -18, 18, 0 }, { "highFreq", "High Freq", 2000, 18000, 8000 }, { "lpf", "High Cut", 1000, 22000, 22000 },
            { "out", "Output", -18, 18, 0 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; for (auto& b : bands) b.clear(); lastHash = -1; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        float hash = 0; for (int i = 0; i < 9; ++i) hash += p (i) * (float) (i + 1) * 1.37f;
        if (hash != lastHash)
        {
            lastHash = hash;
            bands[0].set (Biquad::HighPass, sr, p (0), 0.707, 0);
            bands[1].set (Biquad::LowShelf, sr, p (2), 0.707, p (1));
            bands[2].set (Biquad::Peak, sr, p (4), p (5), p (3));
            bands[3].set (Biquad::HighShelf, sr, p (7), 0.707, p (6));
            bands[4].set (Biquad::LowPass, sr, p (8), 0.707, 0);
        }
        const bool useHp = p (0) > 21, useLp = p (8) < 21000;
        const float g = dbToGain (p (9));
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                if (useHp) x = bands[0].tick (x, ch);
                if (p (1) != 0) x = bands[1].tick (x, ch);
                if (p (3) != 0) x = bands[2].tick (x, ch);
                if (p (6) != 0) x = bands[3].tick (x, ch);
                if (useLp) x = bands[4].tick (x, ch);
                d[i] = x * g;
            }
        }
    }
private:
    double sr = 44100; Biquad bands[5]; float lastHash = -1;
};

// ------------------------------------------------------------ Compressor
class Compressor : public BuiltinEffect
{
public:
    Compressor() { initDefaults(); }
    const char* typeId() const override { return "comp"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "threshold", "Threshold", -60, 0, -18 }, { "ratio", "Ratio", 1, 20, 4 }, { "attack", "Attack ms", 0.1f, 100, 5 },
            { "release", "Release ms", 10, 1000, 120 }, { "knee", "Knee dB", 0, 18, 6 }, { "makeup", "Makeup", -12, 24, 0 },
            { "auto", "Auto Gain", 0, 1, 1, { "Off", "On" } }, { "mix", "Mix", 0, 1, 1 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; env = 0; }
    float gainReductionDb() const { return grDb.load(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float th = p (0), ratio = p (1), knee = p (4);
        const float att = std::exp (-1.0f / (0.001f * p (2) * (float) sr)), rel = std::exp (-1.0f / (0.001f * p (3) * (float) sr));
        float makeup = p (5);
        if (p (6) > 0.5f) makeup += -th * (1.0f - 1.0f / ratio) * 0.5f;
        const float mg = dbToGain (makeup), mix = p (7);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        float maxGr = 0;
        for (int i = 0; i < n; ++i)
        {
            const float lvl = gainToDb (std::max (std::abs (l[i]), std::abs (r[i])));
            float over = lvl - th, gr = 0;
            if (2 * over < -knee) gr = 0;
            else if (2 * std::abs (over) <= knee && knee > 0) gr = (1.0f / ratio - 1.0f) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else gr = over * (1.0f / ratio - 1.0f);
            env = gr < env ? att * env + (1 - att) * gr : rel * env + (1 - rel) * gr;
            maxGr = std::min (maxGr, env);
            const float g = dbToGain (env) * mg;
            l[i] = l[i] * (1 - mix) + l[i] * g * mix;
            r[i] = r[i] * (1 - mix) + r[i] * g * mix;
        }
        grDb.store (maxGr);
    }
private:
    double sr = 44100; float env = 0; std::atomic<float> grDb { 0 };
};

// ------------------------------------------------------------ Limiter (zero latency, fast-attack peak)
class Limiter : public BuiltinEffect
{
public:
    Limiter() { initDefaults(); }
    const char* typeId() const override { return "limiter"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "gain", "Input Gain", 0, 24, 0 }, { "ceiling", "Ceiling", -12, 0, -0.3f }, { "release", "Release ms", 5, 500, 60 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; g = 1; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float in = dbToGain (p (0)), ceil = dbToGain (p (1)), rel = std::exp (-1.0f / (0.001f * p (2) * (float) sr));
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float a = std::max (std::abs (l[i]), std::abs (r[i])) * in;
            const float target = a > ceil ? ceil / a : 1.0f;
            g = target < g ? target : rel * g + (1 - rel) * target;
            l[i] = juce::jlimit (-ceil, ceil, l[i] * in * g);
            r[i] = juce::jlimit (-ceil, ceil, r[i] * in * g);
        }
    }
private:
    double sr = 44100; float g = 1;
};

// ------------------------------------------------------------ Room Verb
class RoomVerb : public BuiltinEffect
{
public:
    RoomVerb() { initDefaults(); }
    const char* typeId() const override { return "verb"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "size", "Size", 0, 1, 0.6f }, { "damp", "Damping", 0, 1, 0.45f }, { "width", "Width", 0, 1, 1 },
                                                 { "mix", "Mix", 0, 1, 0.25f }, { "lowcut", "Low Cut", 20, 1000, 200 } };
        return ps;
    }
    double tailSeconds() const override { return 6.0; }
    void prepare (double s, int maxBlock) override { sr = s; verb.setSampleRate (s); wet.setSize (2, maxBlock); hp.clear(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        juce::Reverb::Parameters rp; rp.roomSize = p (0); rp.damping = p (1); rp.width = p (2); rp.wetLevel = 1; rp.dryLevel = 0;
        verb.setParameters (rp);
        if (wet.getNumSamples() < n) return;
        hp.set (Biquad::HighPass, sr, p (4), 0.707, 0);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* s = b.getReadPointer (ch); auto* w = wet.getWritePointer (ch);
            for (int i = 0; i < n; ++i) w[i] = hp.tick (s[i], ch);
        }
        verb.processStereo (wet.getWritePointer (0), wet.getWritePointer (1), n);
        const float m = p (3);
        for (int ch = 0; ch < 2; ++ch)
        {
            b.applyGain (ch, 0, n, 1.0f - m * 0.5f);
            b.addFrom (ch, 0, wet, ch, 0, n, m);
        }
    }
private:
    double sr = 44100; juce::Reverb verb; juce::AudioBuffer<float> wet; Biquad hp;
};

// ------------------------------------------------------------ Echo Tape (tempo-synced delay)
class EchoTape : public BuiltinEffect
{
public:
    EchoTape() { initDefaults(); }
    const char* typeId() const override { return "delay"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "time", "Time", 0, 6, 2, { "1/32", "1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2" } },
                                                 { "feedback", "Feedback", 0, 0.95f, 0.4f }, { "tone", "Tone", 500, 18000, 6000 },
                                                 { "pingpong", "Ping Pong", 0, 1, 1, { "Off", "On" } }, { "mix", "Mix", 0, 1, 0.25f } };
        return ps;
    }
    double tailSeconds() const override { return 4.0; }
    void prepare (double s, int) override
    { sr = s; size = (int) (s * 4.0); buf.setSize (2, size); buf.clear(); w = 0; lp[0] = lp[1] = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        static const double beats[] { 0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0 };
        const int d = juce::jlimit (1, size - 1, (int) (beats[(int) p (0)] * 60.0 / ctx.bpm * sr));
        const float fb = p (1), mix = p (4), c = 1.0f - std::exp (-2.0f * kPi * p (2) / (float) sr);
        const bool pp = p (3) > 0.5f;
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        auto* bl = buf.getWritePointer (0); auto* br = buf.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            int rd = w - d; if (rd < 0) rd += size;
            float dl = bl[rd], dr = br[rd];
            lp[0] += c * (dl - lp[0]); lp[1] += c * (dr - lp[1]);
            const float inM = 0.5f * (l[i] + r[i]);
            if (pp) { bl[w] = inM + lp[1] * fb; br[w] = lp[0] * fb; }
            else    { bl[w] = l[i] + lp[0] * fb; br[w] = r[i] + lp[1] * fb; }
            l[i] += dl * mix; r[i] += dr * mix;
            if (++w >= size) w = 0;
        }
    }
private:
    double sr = 44100; juce::AudioBuffer<float> buf; int size = 1, w = 0; float lp[2] {};
};

// ------------------------------------------------------------ wrappers around juce::dsp
template <typename DSP>
static void runDsp (DSP& d, juce::AudioBuffer<float>& b, int n)
{
    juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), 2, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> c (blk);
    d.process (c);
}

class Chorus : public BuiltinEffect
{
public:
    Chorus() { initDefaults(); }
    const char* typeId() const override { return "chorus"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "rate", "Rate Hz", 0.05f, 5, 0.8f }, { "depth", "Depth", 0, 1, 0.35f },
                                                 { "delay", "Delay ms", 1, 30, 7 }, { "feedback", "Feedback", -0.9f, 0.9f, 0 }, { "mix", "Mix", 0, 1, 0.5f } };
        return ps;
    }
    void prepare (double s, int mb) override { d.prepare ({ s, (juce::uint32) mb, 2 }); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        d.setRate (p (0)); d.setDepth (p (1)); d.setCentreDelay (p (2)); d.setFeedback (p (3)); d.setMix (p (4));
        runDsp (d, b, n);
    }
private:
    juce::dsp::Chorus<float> d;
};

class PhaseSwirl : public BuiltinEffect
{
public:
    PhaseSwirl() { initDefaults(); }
    const char* typeId() const override { return "phaser"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "rate", "Rate Hz", 0.05f, 8, 0.5f }, { "depth", "Depth", 0, 1, 0.6f },
                                                 { "centre", "Centre", 100, 6000, 1200 }, { "feedback", "Feedback", -0.9f, 0.9f, 0.4f }, { "mix", "Mix", 0, 1, 0.5f } };
        return ps;
    }
    void prepare (double s, int mb) override { d.prepare ({ s, (juce::uint32) mb, 2 }); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        d.setRate (p (0)); d.setDepth (p (1)); d.setCentreFrequency (p (2)); d.setFeedback (p (3)); d.setMix (p (4));
        runDsp (d, b, n);
    }
private:
    juce::dsp::Phaser<float> d;
};

// ------------------------------------------------------------ Grit (saturation / distortion)
class Grit : public BuiltinEffect
{
public:
    Grit() { initDefaults(); }
    const char* typeId() const override { return "grit"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "drive", "Drive", 0, 1, 0.3f }, { "mode", "Mode", 0, 3, 0, { "Tape", "Tube", "Fuzz", "Fold" } },
                                                 { "tone", "Tone", 500, 18000, 9000 }, { "mix", "Mix", 0, 1, 1 }, { "out", "Output", -24, 6, 0 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; lp[0] = lp[1] = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float k = 1.0f + p (0) * 30.0f, mix = p (3), og = dbToGain (p (4)), c = 1.0f - std::exp (-2.0f * kPi * p (2) / (float) sr);
        const int mode = (int) p (1);
        const float norm = 1.0f / std::tanh (k);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                const float x = d[i]; float y;
                switch (mode)
                {
                    case 0: y = std::tanh (k * x) * norm; break;
                    case 1: y = x >= 0 ? std::tanh (k * x) * norm : std::tanh (k * 0.6f * x) * norm * 0.8f; break;
                    case 2: y = juce::jlimit (-1.0f, 1.0f, k * x); break;
                    default: y = std::sin (k * x * 0.5f); break;
                }
                lp[ch] += c * (y - lp[ch]);
                d[i] = (x * (1 - mix) + lp[ch] * mix) * og;
            }
        }
    }
private:
    double sr = 44100; float lp[2] {};
};

// ------------------------------------------------------------ Sweep Filter (SVF with synced LFO)
class SweepFilter : public BuiltinEffect
{
public:
    SweepFilter() { initDefaults(); }
    const char* typeId() const override { return "filter"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "mode", "Mode", 0, 2, 0, { "Low Pass", "High Pass", "Band Pass" } }, { "cutoff", "Cutoff", 20, 20000, 20000 },
                                                 { "res", "Resonance", 0, 0.95f, 0.2f }, { "lfo", "LFO Amount", 0, 1, 0 },
                                                 { "rate", "LFO Rate", 0, 4, 2, { "4 bars", "1 bar", "1/2", "1/4", "1/8" } } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; for (auto& v : ic) v = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        static const double rates[] { 16.0, 4.0, 2.0, 1.0, 0.5 };
        const int mode = (int) p (0);
        const float k = 2.0f - 2.0f * p (2);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        const double beatsPerSample = ctx.bpm / 60.0 / sr;
        for (int i = 0; i < n; i += 32)
        {
            const int m = std::min (32, n - i);
            float cut = p (1);
            if (p (3) > 0)
            {
                const double ph = std::fmod ((ctx.beatPos + i * beatsPerSample) / rates[(int) p (4)], 1.0);
                cut *= std::pow (2.0f, -p (3) * 6.0f * (0.5f - 0.5f * (float) std::cos (ph * 2.0 * juce::MathConstants<double>::pi)));
            }
            const float g = std::tan (kPi * juce::jlimit (20.0f, (float) sr * 0.45f, cut) / (float) sr);
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = ch == 0 ? l : r; float& s1 = ic[ch * 2]; float& s2 = ic[ch * 2 + 1];
                for (int j = i; j < i + m; ++j)
                {
                    const float v3 = d[j] - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
                    s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
                    d[j] = mode == 0 ? v2 : (mode == 1 ? d[j] - k * v1 - v2 : v1);
                }
            }
        }
    }
private:
    double sr = 44100; float ic[4] {};
};

// ------------------------------------------------------------ Glitch Gate (Gross Beat-style 16-step time/volume sequencer)
class GlitchGate : public BuiltinEffect
{
public:
    enum Mode { Thru = 0, Rep4, Rep8, Rep16, Rep32, Reverse, HalfSpeed, TapeStop, Mute, Stutter3 };
    GlitchGate() { initDefaults(); }
    const char* typeId() const override { return "glitch"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<const char*> modes { "Thru", "Repeat 1/4", "Repeat 1/8", "Repeat 1/16", "Repeat 1/32", "Reverse", "Half Speed", "Tape Stop", "Mute", "Triplet Stutter" };
        static const std::vector<ParamDef> ps = []
        {
            std::vector<ParamDef> v;
            static const char* ids[] { "s1","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","s12","s13","s14","s15","s16" };
            static const char* names[] { "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16" };
            for (int i = 0; i < 16; ++i) v.push_back ({ ids[i], names[i], 0, 9, 0, modes });
            v.push_back ({ "steplen", "Step", 0, 2, 1, { "1/8", "1/16", "1/32" } });
            v.push_back ({ "smooth", "Smooth ms", 0.5f, 20, 3 });
            v.push_back ({ "mix", "Mix", 0, 1, 1 });
            return v;
        }();
        return ps;
    }
    void prepare (double s, int) override { sr = s; size = (int) (s * 8.0); ring.setSize (2, size); ring.clear(); w = 0; lastStep = -1; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        static const double stepBeats[] { 0.5, 0.25, 0.125 };
        const double sb = stepBeats[(int) p (16)];
        const double samplesPerBeat = sr * 60.0 / ctx.bpm;
        const double stepSamples = sb * samplesPerBeat;
        const float mix = p (18);
        const float smoothStep = 1.0f / std::max (1.0f, p (17) * 0.001f * (float) sr);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        auto* rl = ring.getWritePointer (0); auto* rr = ring.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            rl[w] = l[i]; rr[w] = r[i];
            const double beat = ctx.beatPos + i / samplesPerBeat;
            const long stepAbs = ctx.playing ? (long) std::floor (beat / sb + 1.0e-9) : -1;
            if (stepAbs != lastStep) { lastStep = stepAbs; stepStart = w; posInStep = 0; }
            const int mode = stepAbs >= 0 ? (int) p ((int) (stepAbs % 16)) : 0;
            float target = 1.0f;
            double readBack = 0.0; // samples behind write head
            switch (mode)
            {
                case Thru: break;
                case Rep4: case Rep8: case Rep16: case Rep32: case Stutter3:
                {
                    const double rb = mode == Stutter3 ? 1.0 / 3.0 : (mode == Rep4 ? 1.0 : mode == Rep8 ? 0.5 : mode == Rep16 ? 0.25 : 0.125);
                    const double repLen = std::max (16.0, rb * samplesPerBeat);
                    readBack = posInStep - std::fmod (posInStep, repLen);
                    break;
                }
                case Reverse:   readBack = 2.0 * posInStep; break;
                case HalfSpeed: readBack = posInStep * 0.5; break;
                case TapeStop:
                {
                    const double t = std::min (1.0, posInStep / stepSamples);
                    readBack = posInStep - (posInStep - posInStep * t * 0.5);
                    readBack = posInStep * t * 0.5;
                    target = (float) (1.0 - t * t);
                    break;
                }
                case Mute: target = 0.0f; break;
                default: break;
            }
            gate += (target - gate) * smoothStep * 4.0f;
            const double rpos = std::fmod ((double) w - readBack + size, (double) size);
            const int i0 = (int) rpos; const int i1 = (i0 + 1) % size; const float f = (float) (rpos - i0);
            const float ol = (rl[i0] + f * (rl[i1] - rl[i0])) * gate;
            const float orr = (rr[i0] + f * (rr[i1] - rr[i0])) * gate;
            // de-click on mode jumps
            const float m = mode == Thru ? 0.0f : 1.0f;
            wetAmt += (m - wetAmt) * smoothStep;
            l[i] = l[i] * (1.0f - wetAmt * mix) * (mode == Thru ? gate : 1.0f) + ol * wetAmt * mix;
            r[i] = r[i] * (1.0f - wetAmt * mix) * (mode == Thru ? gate : 1.0f) + orr * wetAmt * mix;
            posInStep += 1.0;
            if (++w >= size) w = 0;
        }
    }
private:
    double sr = 44100, posInStep = 0; juce::AudioBuffer<float> ring; int size = 1, w = 0, stepStart = 0; long lastStep = -1;
    float gate = 1, wetAmt = 0;
};

// ------------------------------------------------------------ small utilities
class StereoWide : public BuiltinEffect
{
public:
    StereoWide() { initDefaults(); }
    const char* typeId() const override { return "wide"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "width", "Width", 0, 2, 1.3f }, { "monobass", "Mono Bass", 0, 1, 1, { "Off", "On" } } }; return ps; }
    void prepare (double s, int) override { sr = s; lp = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        const float wd = p (0), c = 1.0f - std::exp (-2.0f * kPi * 150.0f / (float) sr);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]); float s = 0.5f * (l[i] - r[i]);
            lp += c * (s - lp);
            s = p (1) > 0.5f ? (s - lp) * wd : s * wd;
            l[i] = m + s; r[i] = m - s;
        }
    }
private:
    double sr = 44100; float lp = 0;
};

class SoftClip : public BuiltinEffect
{
public:
    SoftClip() { initDefaults(); }
    const char* typeId() const override { return "clip"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "gain", "Gain", 0, 24, 0 }, { "ceiling", "Ceiling", -12, 0, -0.5f } }; return ps; }
    void prepare (double, int) override {}
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float g = dbToGain (p (0)), c = dbToGain (p (1));
        for (int ch = 0; ch < 2; ++ch) { auto* d = b.getWritePointer (ch); for (int i = 0; i < n; ++i) d[i] = c * std::tanh (d[i] * g / c); }
    }
};

class BitCrush : public BuiltinEffect
{
public:
    BitCrush() { initDefaults(); }
    const char* typeId() const override { return "crush"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "bits", "Bits", 2, 16, 8 }, { "down", "Downsample", 1, 32, 4 }, { "mix", "Mix", 0, 1, 1 } }; return ps; }
    void prepare (double, int) override { cnt = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float q = std::pow (2.0f, p (0) - 1.0f), mix = p (2); const int ds = (int) p (1);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            if (cnt++ % ds == 0) { hl = std::round (l[i] * q) / q; hr = std::round (r[i] * q) / q; }
            l[i] += (hl - l[i]) * mix; r[i] += (hr - r[i]) * mix;
        }
    }
private:
    int cnt = 0; float hl = 0, hr = 0;
};

class NoiseGate : public BuiltinEffect
{
public:
    NoiseGate() { initDefaults(); }
    const char* typeId() const override { return "gate"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "threshold", "Threshold", -80, 0, -45 }, { "attack", "Attack ms", 0.1f, 20, 1 }, { "release", "Release ms", 5, 500, 80 }, { "range", "Range", -80, 0, -60 } }; return ps; }
    void prepare (double s, int) override { sr = s; env = 0; g = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float th = dbToGain (p (0)), floorG = dbToGain (p (3));
        const float att = std::exp (-1.0f / (0.001f * p (1) * (float) sr)), rel = std::exp (-1.0f / (0.001f * p (2) * (float) sr));
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float a = std::max (std::abs (l[i]), std::abs (r[i]));
            env = std::max (a, env * 0.9995f);
            const float t = env > th ? 1.0f : floorG;
            g = t > g ? att * g + (1 - att) * t : rel * g + (1 - rel) * t;
            l[i] *= g; r[i] *= g;
        }
    }
private:
    double sr = 44100; float env = 0, g = 0;
};

class Pump : public BuiltinEffect
{
public:
    Pump() { initDefaults(); }
    const char* typeId() const override { return "pump"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "depth", "Depth", 0, 1, 0.7f }, { "rate", "Rate", 0, 2, 1, { "1/2", "1/4", "1/8" } }, { "curve", "Release", 0.05f, 1, 0.4f } }; return ps; }
    void prepare (double s, int) override { sr = s; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        static const double rates[] { 2.0, 1.0, 0.5 };
        const double rb = rates[(int) p (1)], spb = sr * 60.0 / ctx.bpm;
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const double ph = std::fmod ((ctx.beatPos + i / spb) / rb, 1.0);
            const float shape = (float) std::min (1.0, ph / p (2));
            const float g = 1.0f - p (0) * (1.0f - shape * shape * (3 - 2 * shape));
            l[i] *= g; r[i] *= g;
        }
    }
private:
    double sr = 44100;
};

class DeEsser : public BuiltinEffect
{
public:
    DeEsser() { initDefaults(); }
    const char* typeId() const override { return "deess"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "freq", "Frequency", 3000, 12000, 6500 }, { "threshold", "Threshold", -50, 0, -28 }, { "range", "Max Cut", 0, 24, 10 } }; return ps; }
    void prepare (double s, int) override { sr = s; split.clear(); env = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        split.set (Biquad::HighPass, sr, p (0), 0.707, 0);
        const float th = p (1), range = p (2), rel = std::exp (-1.0f / (0.04f * (float) sr));
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float hl = split.tick (l[i], 0), hr = split.tick (r[i], 1);
            const float lvl = gainToDb (std::max (std::abs (hl), std::abs (hr)));
            const float cut = juce::jlimit (0.0f, range, (lvl - th) * 0.7f);
            env = cut > env ? cut : rel * env + (1 - rel) * cut;
            const float g = dbToGain (-env) - 1.0f;
            l[i] += hl * g; r[i] += hr * g;
        }
    }
private:
    double sr = 44100; Biquad split; float env = 0;
};

class GainUtil : public BuiltinEffect
{
public:
    GainUtil() { initDefaults(); }
    const char* typeId() const override { return "gain"; }
    const std::vector<ParamDef>& params() const override
    { static const std::vector<ParamDef> ps { { "gain", "Gain", -36, 24, 0 }, { "pan", "Pan", -1, 1, 0 }, { "phase", "Invert", 0, 1, 0, { "Off", "On" } }, { "mono", "Mono", 0, 1, 0, { "Off", "On" } } }; return ps; }
    void prepare (double, int) override {}
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float g = dbToGain (p (0)) * (p (2) > 0.5f ? -1.0f : 1.0f), pan = p (1);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        const float gl = g * std::min (1.0f, 1.0f - pan), gr = g * std::min (1.0f, 1.0f + pan);
        for (int i = 0; i < n; ++i)
        {
            if (p (3) > 0.5f) { const float m = 0.5f * (l[i] + r[i]); l[i] = r[i] = m; }
            l[i] *= gl; r[i] *= gr;
        }
    }
};

// ------------------------------------------------------------ AutoPitch (real-time key/scale tuner)
void PitchShifter::prepare (double sr)
{
    size = (int) (sr * 0.1); buf.assign ((size_t) size, 0.0f); write = 0; phase = 0;
    grain = (float) (sr * 0.03);
}

float PitchShifter::process (float x)
{
    buf[(size_t) write] = x;
    phase += (1.0 - ratio) / grain;
    phase -= std::floor (phase);
    auto tap = [&] (double ph)
    {
        const double d = 2.0 + ph * grain;
        double rp = write - d; if (rp < 0) rp += size;
        const int i0 = (int) rp, i1 = (i0 + 1) % size; const float f = (float) (rp - i0);
        const float w = std::sin (kPi * (float) ph);
        return (buf[(size_t) i0] + f * (buf[(size_t) i1] - buf[(size_t) i0])) * w * w;
    };
    double ph2 = phase + 0.5; if (ph2 >= 1.0) ph2 -= 1.0;
    const float y = tap (phase) + tap (ph2);
    if (++write >= size) write = 0;
    return y;
}

float detectPitchYin (const float* x, int n, double sr, float thresh)
{
    const int maxTau = std::min (n / 2, (int) (sr / 70.0));
    const int minTau = (int) (sr / 1100.0);
    if (maxTau <= minTau + 2) return 0.0f;
    float energy = 0; for (int i = 0; i < n; ++i) energy += x[i] * x[i];
    if (energy / (float) n < 1.0e-6f) return 0.0f;
    std::array<float, 2048> d {};
    const int W = n - maxTau;
    float running = 0;
    d[0] = 1;
    for (int tau = 1; tau < maxTau && tau < (int) d.size(); ++tau)
    {
        float s = 0;
        for (int i = 0; i < W; ++i) { const float df = x[i] - x[i + tau]; s += df * df; }
        running += s;
        d[(size_t) tau] = running > 0 ? s * (float) tau / running : 1.0f;
    }
    for (int tau = minTau; tau < maxTau - 1 && tau < (int) d.size() - 1; ++tau)
    {
        if (d[(size_t) tau] < thresh)
        {
            while (tau + 1 < maxTau && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
            const float a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
            const float den = a + c - 2 * b;
            const float t = (float) tau + (std::abs (den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f);
            return (float) sr / t;
        }
    }
    return 0.0f;
}

class AutoPitch : public BuiltinEffect
{
public:
    AutoPitch() { initDefaults(); }
    const char* typeId() const override { return "autopitch"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "style", "Style", 0, 3, 1, { "Natural", "Classic", "Modern", "Hard Tune" } },
            { "speed", "Retune Speed", 0, 1, 0.75f }, { "humanize", "Humanize", 0, 1, 0.2f },
            { "key", "Key", -1, 11, -1, { "Project", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },
            { "scale", "Scale", -1, 15, -1 }, { "voice", "Voice", 0, 2, 1, { "Low", "Mid", "High" } },
            { "mix", "Mix", 0, 1, 1 } };
        return ps;
    }
    int latencySamples() const override { return 0; }
    void prepare (double s, int) override
    {
        sr = s;
        for (auto& sh : shifters) sh.prepare (s);
        dsFactor = s > 60000 ? 4 : 2;
        analysis.assign (2048, 0.0f); aw = 0; hopCount = 0;
        currentRatio = 1.0f; targetRatio = 1.0f; detectedMidi.store (0); correctedMidi.store (0);
    }
    float detected() const { return detectedMidi.load(); }
    float corrected() const { return correctedMidi.load(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        const int style = (int) p (0);
        float speed = p (1);
        if (style == 3) speed = 1.0f;                          // hard tune: instant
        const int root = p (3) < 0 ? ctx.keyRoot : (int) p (3) - 0;
        const int scale = p (4) < 0 ? ctx.scaleIndex : (int) p (4);
        const float mix = p (6);
        const double dsr = sr / dsFactor;
        const float timeMs = juce::jmap (speed, 400.0f, 1.0f);   // retune glide time
        const float coef = std::exp (-1.0f / (0.001f * timeMs * (float) sr));
        const float humanize = style == 3 ? 0.0f : p (2);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (l[i] + r[i]);
            if ((dsCount++ % dsFactor) == 0)
            {
                analysis[(size_t) aw] = mono; aw = (aw + 1) % (int) analysis.size();
                if (++hopCount >= 256)
                {
                    hopCount = 0;
                    std::array<float, 1024> win {};
                    for (int k = 0; k < 1024; ++k) win[(size_t) k] = analysis[(size_t) ((aw - 1024 + k + (int) analysis.size()) % (int) analysis.size())];
                    const float hz = detectPitchYin (win.data(), 1024, dsr, style == 0 ? 0.12f : 0.18f);
                    if (hz > 0)
                    {
                        const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
                        const int target = snapToScale ((int) std::round (midi), root, scale);
                        float semis = (float) target - midi;
                        if (std::abs (semis) < humanize * 0.6f) semis *= (1.0f - humanize);   // let small natural drift through
                        if (style == 0) semis *= 0.8f;
                        targetRatio = std::pow (2.0f, semis / 12.0f);
                        detectedMidi.store (midi); correctedMidi.store ((float) target);
                    }
                    else targetRatio = 1.0f;
                }
            }
            currentRatio = targetRatio + (currentRatio - targetRatio) * coef;
            for (auto& sh : shifters) sh.setRatio (currentRatio);
            const float tl = shifters[0].process (l[i]), tr = shifters[1].process (r[i]);
            const bool bypassShift = std::abs (currentRatio - 1.0f) < 1.0e-4f && targetRatio == 1.0f;
            const float wl = bypassShift ? l[i] : tl, wr = bypassShift ? r[i] : tr;
            l[i] = l[i] * (1 - mix) + wl * mix;
            r[i] = r[i] * (1 - mix) + wr * mix;
        }
    }
private:
    double sr = 44100; int dsFactor = 2, dsCount = 0, aw = 0, hopCount = 0;
    std::vector<float> analysis; PitchShifter shifters[2];
    float currentRatio = 1, targetRatio = 1;
    std::atomic<float> detectedMidi { 0 }, correctedMidi { 0 };
};

// ------------------------------------------------------------ registry
std::unique_ptr<BuiltinEffect> createStudioEffect (const juce::String& id);

const std::vector<EffectInfo>& builtinEffects()
{
    static const std::vector<EffectInfo> fx {
        { "strip", "Channel Strip", "Studio" }, { "steq7", "Studio EQ 7", "Studio" }, { "stdyn", "Studio Dyn", "Studio" },
        { "fet76", "FET 76", "Studio" }, { "stgate", "Studio Gate/Expander", "Studio" }, { "stlimit", "Studio Limiter", "Studio" },
        { "satur", "Saturator", "Studio" },
        { "autopitch", "AutoPitch", "Vocal" }, { "deess", "De-Esser", "Vocal" },
        { "eq", "Para EQ", "EQ & Filter" }, { "filter", "Sweep Filter", "EQ & Filter" },
        { "comp", "Compressor", "Dynamics" }, { "limiter", "Limiter", "Dynamics" }, { "gate", "Noise Gate", "Dynamics" },
        { "pump", "Pump (Sidechain)", "Dynamics" }, { "clip", "Soft Clipper", "Dynamics" },
        { "verb", "Room Verb", "Space" }, { "delay", "Echo Tape", "Space" }, { "chorus", "Chorus", "Modulation" },
        { "phaser", "Phase Swirl", "Modulation" }, { "wide", "Stereo Wide", "Utility" }, { "gain", "Gain / Pan", "Utility" },
        { "grit", "Grit", "Distortion" }, { "crush", "Bit Crush", "Distortion" },
        { "glitch", "Glitch Gate", "Time FX" } };
    return fx;
}

std::unique_ptr<BuiltinEffect> createBuiltinEffect (const juce::String& id)
{
    if (auto s = createStudioEffect (id)) return s;
    if (id == "eq") return std::make_unique<ParaEQ>();
    if (id == "comp") return std::make_unique<Compressor>();
    if (id == "limiter") return std::make_unique<Limiter>();
    if (id == "verb") return std::make_unique<RoomVerb>();
    if (id == "delay") return std::make_unique<EchoTape>();
    if (id == "chorus") return std::make_unique<Chorus>();
    if (id == "phaser") return std::make_unique<PhaseSwirl>();
    if (id == "grit") return std::make_unique<Grit>();
    if (id == "filter") return std::make_unique<SweepFilter>();
    if (id == "glitch") return std::make_unique<GlitchGate>();
    if (id == "wide") return std::make_unique<StereoWide>();
    if (id == "clip") return std::make_unique<SoftClip>();
    if (id == "crush") return std::make_unique<BitCrush>();
    if (id == "gate") return std::make_unique<NoiseGate>();
    if (id == "pump") return std::make_unique<Pump>();
    if (id == "deess") return std::make_unique<DeEsser>();
    if (id == "autopitch") return std::make_unique<AutoPitch>();
    if (id == "gain") return std::make_unique<GainUtil>();
    return {};
}
}
