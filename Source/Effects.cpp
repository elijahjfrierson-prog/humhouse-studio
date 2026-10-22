#include "Effects.h"
#include "rockglue/RockGlueEngine.h"
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
    if (i >= 0) { auto& d = params()[(size_t) i]; values[(size_t) i].store (juce::jlimit (d.min, d.max, v)); paramChanged (i); }
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
    float meter (int) const override { return grDb.load(); }
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
                                                 { "mix", "Mix", 0, 1, 0.25f }, { "lowcut", "Low Cut", 20, 1000, 200 },
                                                 { "reverse", "Reverse", 0, 1, 0, { "Off", "On" } },
                                                 { "revlen", "Rev Length", 0, 3, 1, { "1/4", "1/2", "1 Bar", "2 Bars" } } };
        return ps;
    }
    double tailSeconds() const override { return p (5) > 0.5f ? 14.0 : 6.0; }
    void prepare (double s, int maxBlock) override
    {
        sr = s; verb.setSampleRate (s); wet.setSize (2, maxBlock); hp.clear();
        revReady.store (false); prepared = true;
        if (p (5) > 0.5f) allocRev();
    }
    // Reverse memory is only made when Reverse is on, and never on the audio thread
    void paramChanged (int i) override { if (i == 5 && prepared && p (5) > 0.5f) allocRev(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
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
        if (p (5) > 0.5f && revReady.load (std::memory_order_acquire)) reverse (n, ctx);
        const float m = p (3);
        for (int ch = 0; ch < 2; ++ch)
        {
            b.applyGain (ch, 0, n, 1.0f - m * 0.5f);
            b.addFrom (ch, 0, wet, ch, 0, n, m);
        }
    }
private:
    void allocRev()
    {
        if (revReady.load()) return;
        revCap = (int) (sr * 8.0);
        rev.setSize (2, 2 * revCap); rev.clear();
        segLen = 0;
        revReady.store (true, std::memory_order_release);
    }
    // tempo-synced reverse: each beat-locked chunk of reverb plays backwards in the next one, so it swells into the beat
    void reverse (int n, const PlayContext& ctx)
    {
        static constexpr double beatsOf[] { 1.0, 2.0, 4.0, 8.0 };
        const double beats = beatsOf[juce::jlimit (0, 3, (int) p (6))];
        const int L = juce::jlimit (256, revCap, (int) (beats * 60.0 / juce::jmax (20.0, ctx.bpm) * sr));
        if (L != segLen) { segLen = L; segPos = 0; half = 0; silent = 1; }   // the other half holds a different length: skip one chunk
        if (ctx.playing)
        {
            const int pos = juce::jlimit (0, L - 1, (int) (std::fmod (juce::jmax (0.0, ctx.beatPos), beats) / beats * L));
            const int d = std::abs (pos - segPos);
            if (juce::jmin (d, L - d) > 64) silent = 2;            // jumped (seek / loop): skip the stale chunk
            if (segPos > L / 2 && pos < L / 2) half ^= 1;           // crossed a chunk edge the grid already crossed
            else if (segPos < L / 2 && pos > L / 2 && d > L / 2) half ^= 1;
            segPos = pos;
        }
        const int fade = juce::jmax (1, juce::jmin (L / 4, (int) (0.005 * sr)));
        auto* w0 = wet.getWritePointer (0); auto* w1 = wet.getWritePointer (1);
        auto* r0 = rev.getWritePointer (0); auto* r1 = rev.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const int wi = half * revCap + segPos, ri = (1 - half) * revCap + (L - 1 - segPos);
            const float g = silent > 0 ? 0.0f
                          : segPos < fade ? (float) segPos / (float) fade
                          : L - segPos < fade ? (float) (L - segPos) / (float) fade : 1.0f;
            r0[wi] = w0[i]; r1[wi] = w1[i];
            w0[i] = r0[ri] * g; w1[i] = r1[ri] * g;
            if (++segPos >= L) { segPos = 0; half ^= 1; if (silent > 0) --silent; }
        }
    }
    double sr = 44100; juce::Reverb verb; juce::AudioBuffer<float> wet; Biquad hp;
    juce::AudioBuffer<float> rev; std::atomic<bool> revReady { false }; bool prepared = false;
    int revCap = 0, segLen = 0, segPos = 0, half = 0, silent = 0;
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

// modulated short delay used by Flanger and Flangus (0 latency: wet taps only look back in time)
struct ModTap
{
    std::vector<float> buf; int w = 0, size = 0;
    void prepare (double sr) { size = (int) (sr * 0.05) + 4; buf.assign ((size_t) size, 0.0f); w = 0; }
    void push (float x) { buf[(size_t) w] = x; if (++w >= size) w = 0; }
    float read (float delaySamples) const
    {
        float r = (float) w - 1.0f - juce::jlimit (0.0f, (float) size - 3.0f, delaySamples);
        while (r < 0) r += (float) size;
        const int i0 = (int) r; const float fr = r - (float) i0;
        const int i1 = (i0 + 1) % size;
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr;
    }
};

class Flanger : public BuiltinEffect
{
public:
    Flanger() { initDefaults(); }
    const char* typeId() const override { return "flanger"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "rate", "Rate Hz", 0.02f, 10, 0.25f }, { "depth", "Depth", 0, 1, 0.7f },
                                                 { "delay", "Delay ms", 0.1f, 10, 1.5f }, { "feedback", "Feedback", -0.95f, 0.95f, 0.5f },
                                                 { "stereo", "Stereo (deg)", 0, 180, 90 }, { "invert", "Polarity", 0, 1, 0, { "Normal", "Inverted (zero-style)" } },
                                                 { "mix", "Mix", 0, 1, 0.5f } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; for (auto& t : taps) t.prepare (s); fb[0] = fb[1] = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float rate = p (0), depth = p (1), dMs = p (2), feed = p (3), mix = p (6);
        const float off = p (4) / 360.0f, sign = p (5) > 0.5f ? -1.0f : 1.0f;
        const float inc = rate / (float) sr;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < juce::jmin (2, b.getNumChannels()); ++c)
            {
                float ph = phase + (c == 1 ? off : 0.0f); ph -= std::floor (ph);
                const float lfo = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ph);
                const float d = (0.05f + dMs * (1.0f - depth * lfo)) * 0.001f * (float) sr;
                auto* x = b.getWritePointer (c);
                const float wet = taps[c].read (d);
                taps[c].push (x[i] + feed * fb[c]);
                fb[c] = wet;
                x[i] = x[i] * (1.0f - 0.5f * mix) + sign * wet * mix * 0.5f * 1.4f;
            }
            phase += inc; if (phase >= 1.0f) phase -= 1.0f;
        }
    }
private:
    double sr = 44100; ModTap taps[2]; float fb[2] {}, phase = 0;
};

// FL Flangus-style multi-voice flanger / chorus: voices, spread, cross feedback
class Flangus : public BuiltinEffect
{
public:
    Flangus() { initDefaults(); }
    const char* typeId() const override { return "flangus"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "voices", "Order (voices)", 1, 6, 2 }, { "depth", "Depth", 0, 1, 0.5f },
                                                 { "speed", "Speed Hz", 0.02f, 5, 0.3f }, { "delay", "Delay ms", 0.5f, 20, 3 },
                                                 { "spread", "Spread", 0, 1, 0.6f }, { "cross", "Cross", 0, 1, 0.3f },
                                                 { "feedback", "Feedback", -0.9f, 0.9f, 0.2f }, { "mix", "Dry / Wet", 0, 1, 0.5f } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; for (auto& t : taps) t.prepare (s); fb[0] = fb[1] = 0; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const int nv = juce::jlimit (1, 6, (int) std::round (p (0)));
        const float depth = p (1), dMs = p (3), spread = p (4), cross = p (5), feed = p (6), mix = p (7);
        const float inc = p (2) / (float) sr;
        const bool st = b.getNumChannels() > 1;
        auto* L = b.getWritePointer (0); auto* R = st ? b.getWritePointer (1) : L;
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];
            taps[0].push (inL + feed * ((1.0f - cross) * fb[0] + cross * fb[1]));
            taps[1].push (inR + feed * ((1.0f - cross) * fb[1] + cross * fb[0]));
            float wl = 0, wr = 0;
            for (int v = 0; v < nv; ++v)
            {
                float ph = phase + (float) v / (float) nv; ph -= std::floor (ph);
                const float lfo = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ph);
                const float d = (0.1f + dMs * (1.0f - depth * lfo)) * 0.001f * (float) sr;
                const float pan = nv == 1 ? 0.0f : spread * (2.0f * (float) v / (float) (nv - 1) - 1.0f);
                const float a = taps[0].read (d), c = taps[1].read (d);
                wl += a * (1.0f - juce::jmax (0.0f, pan)); wr += c * (1.0f + juce::jmin (0.0f, pan));
            }
            const float norm = 1.0f / std::sqrt ((float) nv);
            // feedback uses the voice average (loop gain stays < 1 even when voices line up) and a soft limit, so it can't run away
            auto soft = [] (float x) { return std::isfinite (x) ? std::tanh (x) : 0.0f; };
            fb[0] = soft (wl / (float) nv); fb[1] = soft (wr / (float) nv);
            wl *= norm; wr *= norm;
            L[i] = inL * (1.0f - 0.5f * mix) + wl * mix * 0.7f;
            if (st) R[i] = inR * (1.0f - 0.5f * mix) + wr * mix * 0.7f;
            phase += inc; if (phase >= 1.0f) phase -= 1.0f;
        }
    }
private:
    double sr = 44100; ModTap taps[2]; float fb[2] {}, phase = 0;
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

// ------------------------------------------------------------ Glitch Gate (Gross Beat + ShaperBox style: one-click time / volume / filter / pan shapes)
class GlitchGate : public BuiltinEffect
{
public:
    enum Mode { Thru = 0, Rep4, Rep8, Rep16, Rep32, Reverse, HalfSpeed, TapeStop, Mute, Stutter3 };
    enum Fx { Steps = 0, FxTapeStop, FxTapeStart, FxStut8, FxStut16, FxStutTrip, FxRoll, FxScratch, FxBaby, FxReverse, FxRewind, FxHalf,
              FxGate8, FxGate16, FxTrance, FxPump, FxSweep, FxWobble, FxPan, FxCrush, FxDrop, NumFx };
    static constexpr int iSmooth = 17, iMix = 18, iFx = 19, iLen = 20, iDepth = 21;
    GlitchGate() { initDefaults(); }
    const char* typeId() const override { return "glitch"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<const char*> modes { "Thru", "Repeat 1/4", "Repeat 1/8", "Repeat 1/16", "Repeat 1/32", "Reverse", "Half Speed", "Tape Stop", "Mute", "Triplet Stutter" };
        static const std::vector<const char*> fxs { "Steps", "Tape Stop", "Tape Start", "Stutter 1/8", "Stutter 1/16", "Triplet Stutter", "Stutter Roll",
                                                     "Scratch", "Baby Scratch", "Reverse", "Rewind", "Half Speed", "Gate 1/8", "Gate 1/16", "Trance Gate",
                                                     "Sidechain Pump", "Filter Sweep", "Wobble", "Pan Swirl", "Lo-Fi Crush", "Drop" };
        static const std::vector<ParamDef> ps = []
        {
            std::vector<ParamDef> v;
            static const char* ids[] { "s1","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","s12","s13","s14","s15","s16" };
            static const char* names[] { "1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16" };
            for (int i = 0; i < 16; ++i) v.push_back ({ ids[i], names[i], 0, 9, 0, modes });
            v.push_back ({ "steplen", "Step", 0, 2, 1, { "1/8", "1/16", "1/32" } });
            v.push_back ({ "smooth", "Smooth ms", 0.5f, 20, 3 });
            v.push_back ({ "mix", "Mix", 0, 1, 1 });
            v.push_back ({ "fx", "Effect", 0, (float) (NumFx - 1), (float) FxStut8, fxs });
            v.push_back ({ "length", "Length", 0, 3, 2, { "1 beat", "2 beats", "1 bar", "2 bars" } });
            v.push_back ({ "depth", "Depth", 0, 1, 1 });
            return v;
        }();
        return ps;
    }
    void prepare (double s, int) override
    {
        sr = s; size = (int) (s * 16.0); ring.setSize (2, size); ring.clear(); w = 0; lastStep = -1;
        prevRb = oldRb = 0; xf = 0; vs = 1; gl = gr = 1; fcS = 20000; ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; holdN = 0; freeBeat = 0;
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        const double spb = sr * 60.0 / juce::jmax (20.0, ctx.bpm);
        const double beat0 = ctx.playing ? ctx.beatPos : freeBeat;   // stopped: keeps running on its own clock so you hear it
        freeBeat = beat0 + n / spb;
        const int fx = juce::jlimit (0, NumFx - 1, (int) std::round (p (iFx)));
        if (fx == Steps) processSteps (b, n, beat0, spb, ctx.playing);
        else processShape (b, n, beat0, spb, fx);
    }
private:
    float readHead (const float* buf, double rb) const
    {
        const double rpos = std::fmod ((double) w - rb + 2.0 * size, (double) size);
        const int i0 = (int) rpos, i1 = (i0 + 1) % size; const float f = (float) (rpos - i0);
        return buf[i0] + f * (buf[i1] - buf[i0]);
    }
    void processShape (juce::AudioBuffer<float>& b, int n, double beat0, double spb, int fx)
    {
        static const double lens[] { 1.0, 2.0, 4.0, 8.0 };
        static const int trance[16] { 1,0,1,1, 0,1,1,0, 1,0,1,1, 0,1,0,1 };
        const double cyc = lens[juce::jlimit (0, 3, (int) std::round (p (iLen)))];
        const float depth = p (iDepth), mix = p (iMix);
        const float sm = 1.0f / std::max (1.0f, p (iSmooth) * 0.001f * (float) sr);
        const double xfStep = 1.0 / std::max (16.0, std::max (4.0, (double) p (iSmooth)) * 0.001 * sr);
        const double maxRb = size - 8.0;
        const bool timeFx = fx <= FxHalf || fx == FxDrop;
        const bool useFilter = fx == FxSweep || fx == FxWobble || fx == FxCrush || fx == FxDrop;
        const float fsm = 1.0f / (0.02f * (float) sr);
        if (fx != lastFx) { lastFx = fx; lf = std::log (20000.0f); ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        auto* rl = ring.getWritePointer (0); auto* rr = ring.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            rl[w] = l[i]; rr[w] = r[i];
            const double beat = beat0 + i / spb;
            const double cpos = beat - cyc * std::floor (beat / cyc);     // beats into the cycle
            const double bpos = beat - std::floor (beat);                 // beats into the beat
            const double half = cyc * 0.5;
            double rb = 0.0; float vol = 1.0f, fc = 20000.0f, pan = 0.0f; bool crush = false;
            auto repeat = [&] (double sinceBeats, double sliceBeats) { const double u = sinceBeats * spb, L = std::max (16.0, sliceBeats * spb); return u - std::fmod (u, L); };
            switch (fx)
            {
                case FxTapeStop:
                    if (cpos >= half) { const double T = half * spb, u = (cpos - half) * spb, x = u / T; rb = u * u / (2.0 * T); vol = (float) (1.0 - x * x * x * x); }
                    break;
                case FxTapeStart:
                {
                    const double T = cyc * 0.25 * spb, u = cpos * spb;
                    rb = u < T ? u - u * u / (2.0 * T) : T * 0.5;
                    if (u < T) vol = (float) std::sqrt (u / T);
                    break;
                }
                case FxStut8:    rb = repeat (bpos, 0.5); break;
                case FxStut16:   rb = repeat (bpos, 0.25); break;
                case FxStutTrip: rb = repeat (bpos, 1.0 / 3.0); break;
                case FxRoll:
                {
                    const double q = cpos / cyc * 4.0;
                    if (q >= 2.0) rb = repeat (cpos - half, q < 3.0 ? 0.5 : q < 3.5 ? 0.25 : 0.125);
                    break;
                }
                case FxScratch:
                    if (cpos >= half) { const double u = (cpos - half) * spb, wv = juce::MathConstants<double>::twoPi / spb; rb = u - std::sin (wv * u) / wv; }
                    break;
                case FxBaby:
                {
                    const double u = bpos * spb, wv = juce::MathConstants<double>::twoPi * 2.0 / spb;
                    rb = 0.9 * (u - std::sin (wv * u) / wv);
                    break;
                }
                case FxReverse: if (cpos >= half) rb = 2.0 * (cpos - half) * spb; break;
                case FxRewind:  if (cpos >= half) rb = 2.5 * (cpos - half) * spb; break;
                case FxHalf:    rb = 0.5 * cpos * spb; break;
                case FxDrop:    if (cpos >= half) { rb = 0.5 * (cpos - half) * spb; fc = 900.0f; } break;
                case FxGate8:   vol = std::fmod (cpos, 0.5) < 0.3 ? 1.0f : 0.0f; break;
                case FxGate16:  vol = std::fmod (cpos, 0.25) < 0.15 ? 1.0f : 0.0f; break;
                case FxTrance:  { const double s16 = cpos / 0.25; vol = trance[(int) s16 % 16] && (s16 - std::floor (s16)) < 0.8 ? 1.0f : 0.0f; break; }
                case FxPump:    { const double x = 1.0 - bpos; vol = (float) (1.0 - x * x * x); break; }
                case FxSweep:   { const double t = cpos / cyc; fc = (float) (200.0 * std::pow (100.0, t < 0.75 ? t / 0.75 : (1.0 - t) / 0.25)); break; }
                case FxWobble:  fc = (float) (250.0 * std::pow (24.0, 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * 2.0 * bpos))); break;
                case FxPan:     pan = (float) std::sin (juce::MathConstants<double>::twoPi * cpos / 2.0); break;
                case FxCrush:   crush = true; fc = 7000.0f; break;
                default: break;
            }
            rb = juce::jlimit (0.0, maxRb, rb);
            // a jump in the read position (new slice, loop restart) crossfades from the old head instead of clicking
            if (std::abs (rb - prevRb) > 3.0) { oldRb = prevRb; xf = 1.0; }
            prevRb = rb;
            float wl = l[i], wr = r[i];
            if (timeFx)
            {
                float hl = readHead (rl, rb), hr = readHead (rr, rb);
                if (xf > 0.0)
                {
                    const float o = (float) xf;
                    hl = hl * (1.0f - o) + readHead (rl, oldRb) * o;
                    hr = hr * (1.0f - o) + readHead (rr, oldRb) * o;
                    xf -= xfStep;
                }
                wl += (hl - wl) * depth; wr += (hr - wr) * depth;
            }
            vs += ((1.0f - depth * (1.0f - vol)) - vs) * sm * 4.0f;
            wl *= vs; wr *= vs;
            if (useFilter)
            {
                lf += (std::log (20000.0f * std::pow (fc / 20000.0f, depth)) - lf) * fsm;   // log-smoothed cutoff: sweeps, never jumps
                fcS = std::exp (lf);
                if ((i & 15) == 0 || fg <= 0.0f) fg = (float) std::tan (juce::MathConstants<double>::pi * std::min (fcS, 0.45f * (float) sr) / sr);
                const float k = 1.0f / 1.1f, a1 = 1.0f / (1.0f + fg * (fg + k)), a2 = fg * a1, a3 = fg * a2;
                float* ch[2] { &wl, &wr };
                for (int c = 0; c < 2; ++c)
                {
                    const float v3 = *ch[c] - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
                    ic1[c] = 2.0f * v1 - ic1[c]; ic2[c] = 2.0f * v2 - ic2[c];
                    *ch[c] = v2;
                }
            }
            if (crush && depth > 0.0f)
            {
                if (holdN-- <= 0) { holdN = 1 + (int) (5.0f * depth); holdL = wl; holdR = wr; }
                const float q = std::pow (2.0f, 12.0f - 6.0f * depth);
                wl = std::round (holdL * q) / q; wr = std::round (holdR * q) / q;
            }
            const float pn = pan * depth;
            const float tl = std::sqrt (std::max (0.0f, 1.0f - pn)), tr = std::sqrt (std::max (0.0f, 1.0f + pn));
            gl += (tl - gl) * sm; gr += (tr - gr) * sm;
            wl *= gl; wr *= gr;
            l[i] += (wl - l[i]) * mix; r[i] += (wr - r[i]) * mix;
            if (++w >= size) w = 0;
        }
    }
    void processSteps (juce::AudioBuffer<float>& b, int n, double beat0, double samplesPerBeat, bool)
    {
        static const double stepBeats[] { 0.5, 0.25, 0.125 };
        const double sb = stepBeats[juce::jlimit (0, 2, (int) p (16))];
        const double stepSamples = sb * samplesPerBeat;
        const float mix = p (iMix);
        const float smoothStep = 1.0f / std::max (1.0f, p (iSmooth) * 0.001f * (float) sr);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        auto* rl = ring.getWritePointer (0); auto* rr = ring.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            rl[w] = l[i]; rr[w] = r[i];
            const double beat = beat0 + i / samplesPerBeat;
            const long stepAbs = (long) std::floor (beat / sb + 1.0e-9);
            if (stepAbs != lastStep) { lastStep = stepAbs; posInStep = 0; }
            const int mode = (int) p ((int) (((stepAbs % 16) + 16) % 16));
            float target = 1.0f;
            double readBack = 0.0;
            switch (mode)
            {
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
                    readBack = posInStep * t * 0.5;
                    target = (float) (1.0 - t * t);
                    break;
                }
                case Mute: target = 0.0f; break;
                default: break;
            }
            if (std::abs (readBack - prevRb) > 3.0) { oldRb = prevRb; xf = 1.0; }
            prevRb = readBack;
            vs += (target - vs) * smoothStep * 4.0f;
            float ol = readHead (rl, readBack), orr = readHead (rr, readBack);
            if (xf > 0.0)
            {
                const float o = (float) xf;
                ol = ol * (1.0f - o) + readHead (rl, oldRb) * o; orr = orr * (1.0f - o) + readHead (rr, oldRb) * o;
                xf -= smoothStep;
            }
            l[i] += (ol * vs - l[i]) * mix;
            r[i] += (orr * vs - r[i]) * mix;
            posInStep += 1.0;
            if (++w >= size) w = 0;
        }
    }
    double sr = 44100, posInStep = 0, freeBeat = 0, prevRb = 0, oldRb = 0, xf = 0;
    juce::AudioBuffer<float> ring; int size = 1, w = 0, holdN = 0; long lastStep = -1;
    int lastFx = -1;
    float lf = 9.903f, vs = 1, gl = 1, gr = 1, fcS = 20000, fg = 0, ic1[2] {}, ic2[2] {}, holdL = 0, holdR = 0;
};

// ------------------------------------------------------------ Hum House Master (RockGlue architecture, master-bus mode):
// Anchor (mono-locked lows, grit) -> M/S Pocket -> parallel FET smash -> VCA glue. Zero latency.
class HumHouseMaster : public BuiltinEffect
{
public:
    HumHouseMaster() { initDefaults(); }
    const char* typeId() const override { return "hhmaster"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "drive", "Smash Drive", 0, 30, 8 }, { "smashmix", "Parallel Mix", 0, 100, 50 },
            { "monolock", "Mono Lock", 0, 1, 1, { "Off", "On" } }, { "grit", "Grit", 0, 100, 25 },
            { "pocket", "Carve Pocket", 0, 100, 100 }, { "threshold", "Glue Threshold", -20, 10, -10 },
            { "makeup", "Makeup", 0, 12, 2 }, { "autorel", "Auto Release", 0, 1, 1, { "Off", "On" } } };
        return ps;
    }
    void prepare (double s, int) override { engine.prepare (s); engine.reset(); }
    void reset() override { engine.reset(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        engine.setDrumDriveDb (p (0));
        engine.setDrumParallelMix (p (1) * 0.01f);
        engine.setMonoLock (p (2) > 0.5f);
        engine.setGrit (p (3) * 0.01f);
        engine.setCarvePocket (p (4) * 0.01f);
        engine.setGlueThresholdDb (p (5));
        engine.setGlueMakeupDb (p (6));
        engine.setGlueAutoRelease (p (7) > 0.5f);
        engine.processMaster ({ b.getWritePointer (0), b.getWritePointer (1) }, n);
        gr.store (-engine.getMeterFrame().glueGrDb, std::memory_order_relaxed);
    }
    float meter (int) const override { return gr.load (std::memory_order_relaxed); }
private:
    rockglue::RockGlueEngine engine;
    std::atomic<float> gr { 0.0f };
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
void SpliceShifter::prepare (double s)
{
    sr = s;
    int n = 1; while (n < (int) (s * 0.25)) n <<= 1;
    bufL.assign ((size_t) n, 0.0f); bufR.assign ((size_t) n, 0.0f);
    mask = n - 1; w = 0; d = 0.0015 * s; d2 = d; xfLen = xfPos = 0;
}

float SpliceShifter::read (const std::vector<float>& b, double delay) const
{
    const double pos = (double) w - delay, fl = std::floor (pos);
    const int i = (int) fl; const float t = (float) (pos - fl);
    const float y0 = b[(size_t) ((i - 1) & mask)], y1 = b[(size_t) (i & mask)], y2 = b[(size_t) ((i + 1) & mask)], y3 = b[(size_t) ((i + 2) & mask)];
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

// best delay near `target`: the spot whose recent waveform matches what the head just played
double SpliceShifter::findSplice (double target, int span, int len) const
{
    const int cur = (int) std::lround (d);
    float xx = 1.0e-9f;
    for (int j = 0; j < len; ++j) { const size_t a = (size_t) ((w - cur - j) & mask); const float x = bufL[a] + bufR[a]; xx += x * x; }
    double best = target; float bestScore = -2.0f;
    for (int k = -span; k <= span; ++k)
    {
        const int cand = (int) std::lround (target) + k;
        if (cand < 4 || cand + len + 8 > mask) continue;
        float xy = 0, yy = 1.0e-9f;
        for (int j = 0; j < len; ++j)
        {
            const size_t a = (size_t) ((w - cur - j) & mask), c = (size_t) ((w - cand - j) & mask);
            const float x = bufL[a] + bufR[a], y = bufL[c] + bufR[c];
            xy += x * y; yy += y * y;
        }
        const float score = xy / std::sqrt (xx * yy);
        if (score > bestScore) { bestScore = score; best = cand + (d - cur); }
    }
    return best;
}

void SpliceShifter::process (float& l, float& r, double ratio, double period)
{
    bufL[(size_t) w] = l; bufR[(size_t) w] = r;
    const double P = period > 8.0 ? period : 0.008 * sr;
    if (xfLen == 0)
    {
        const int X = juce::jlimit (16, (int) (0.0025 * sr), (int) (P * 0.5));
        const int span = std::max (2, (int) (P / 6.0));
        const double lo = 4.0 + std::max (0.0, ratio - 1.0) * X, hi = lo + P + span + 2.0;
        if (d < lo || d > hi)
        {
            const double target = d < lo ? d + P : d - P;
            d2 = findSplice (target, span, X);
            if (d2 < 4.0) d2 = target;
            // pitching up, the old head catches the input: shorten the crossfade so it never overtakes
            xfLen = ratio > 1.0 ? juce::jlimit (1, X, (int) ((d - 3.0) / (ratio - 1.0))) : X; xfPos = 0;
        }
    }
    float yl = read (bufL, d), yr = read (bufR, d);
    if (xfLen > 0)
    {
        const float g = 0.5f - 0.5f * std::cos (kPi * (float) xfPos / (float) xfLen);
        yl += g * (read (bufL, d2) - yl); yr += g * (read (bufR, d2) - yr);
    }
    const double step = 1.0 - ratio;
    d += step;
    if (xfLen > 0) { d2 += step; if (++xfPos >= xfLen) { d = d2; xfLen = 0; } }
    d = juce::jlimit (2.0, (double) (mask / 2), d);
    w = (w + 1) & mask;
    l = yl; r = yr;
}

void PsolaShifter::prepare (double s)
{
    sr = s;
    int sz = 1; while (sz < (int) (s * 0.5)) sz <<= 1;
    for (auto* v : { &inL, &inR, &outL, &outR, &wsum }) v->assign ((size_t) sz, 0.0f);
    mask = sz - 1; n = 0; ts = 0; mark = 0; lastP = 1; lat = latS = 0.006 * s; dryL = dryR = 0;
}

float PsolaShifter::read (const std::vector<float>& b, double pos) const
{
    const double fl = std::floor (pos);
    const long long i = (long long) fl; const float t = (float) (pos - fl);
    const float y0 = b[(size_t) ((i - 1) & mask)], y1 = b[(size_t) (i & mask)], y2 = b[(size_t) ((i + 1) & mask)], y3 = b[(size_t) ((i + 2) & mask)];
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void PsolaShifter::process (float& l, float& r, double ratio, double period)
{
    inL[(size_t) (n & mask)] = l; inR[(size_t) (n & mask)] = r;
    const double P = juce::jlimit (0.0015 * sr, 0.02 * sr, period > 8.0 ? period : 0.006 * sr);
    ratio = juce::jlimit (0.25, 4.0, ratio);
    if (ts < (double) n) ts = (double) n;
    // the cycle got longer (a lower note): the next grain would start in the past and fade in at half
    // weight = a step in the output (the squeak on low note starts). Start it now from zero weight instead.
    if (ts - P < (double) n - 1.0) ts = (double) n + P;
    while (ts - P <= (double) n)   // next grain starts now: lay it out centred on ts
    {
        lat += juce::jlimit (-0.05 * P, 0.05 * P, 2.3 * P - lat);              // latency follows the voice slowly (no skips)
        const double want = ts - lat;
        double c = mark + P * std::round ((want - mark) / P);                   // stay on the voice's cycle grid
        // a new note with a different cycle length (or the first grain): restart the grid right where the voice
        // is, so the new grains line up with the last ones (a snapped grid here was the squeak on low note starts)
        if (std::abs (c - want) > P || std::abs (P - lastP) > 0.15 * lastP) c = want;
        lastP = P;
        while (c + P > (double) n - 1.0) c -= P;                                // only use input we already have
        mark = c;
        for (long long j = (long long) std::ceil (ts - P); j <= (long long) std::floor (ts + P); ++j)
        {
            if (j < n) continue;
            const double off = (double) j - ts;
            const float w = 0.5f + 0.5f * std::cos (kPi * (float) (off / P));
            const size_t o = (size_t) (j & mask);
            outL[o] += w * read (inL, c + off); outR[o] += w * read (inR, c + off); wsum[o] += w;
        }
        ts += P / ratio;
    }
    const size_t o = (size_t) (n & mask);
    const float g = 1.0f / std::max (0.5f, wsum[o]);
    l = outL[o] * g; r = outR[o] * g;
    outL[o] = outR[o] = wsum[o] = 0.0f;
    latS += juce::jlimit (-0.04, 0.04, lat - latS);   // dry delay glides (lat steps per grain = clicks in the dry path)
    dryL = read (inL, (double) n - latS); dryR = read (inR, (double) n - latS);
    ++n;
}

float detectPitchYin (const float* x, int n, double sr, float thresh, float fallback, float loHz, float hiHz, bool* rough, float* aperiodic)
{
    if (rough != nullptr) *rough = false;
    if (aperiodic != nullptr) *aperiodic = 1.0f;
    const int maxTau = std::min (n / 2, (int) (sr / loHz));
    const int minTau = std::max (2, (int) (sr / hiHz));
    if (maxTau <= minTau + 2) return 0.0f;
    float energy = 0; for (int i = 0; i < n; ++i) energy += x[i] * x[i];
    if (energy / (float) n < 1.0e-6f) return 0.0f;
    std::array<float, 2048> d {};
    const int W = n - maxTau;
    float running = 0;
    d[0] = 1;
    // difference function computed lag by lag, stopping at the first dip under the threshold (same answer as
    // computing every lag, but high and mid voices only need a fraction of the lags = much less CPU)
    const int lim = std::min (maxTau, (int) d.size());
    auto diff = [&] (int tau)
    {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        const float* a = x; const float* b = x + tau;
        int i = 0;
        for (; i + 4 <= W; i += 4)
        {
            const float d0 = a[i] - b[i], d1 = a[i + 1] - b[i + 1], d2 = a[i + 2] - b[i + 2], d3 = a[i + 3] - b[i + 3];
            s0 += d0 * d0; s1 += d1 * d1; s2 += d2 * d2; s3 += d3 * d3;
        }
        for (; i < W; ++i) { const float df = a[i] - b[i]; s0 += df * df; }
        const float s = (s0 + s1) + (s2 + s3);
        running += s;
        d[(size_t) tau] = running > 0 ? s * (float) tau / running : 1.0f;
    };
    for (int tau = 1; tau < lim; ++tau)
    {
        diff (tau);
        int c = tau - 1;   // first local minimum below the threshold
        if (c >= minTau && c >= 1 && d[(size_t) c] < thresh && d[(size_t) tau] >= d[(size_t) c])
        {
            // a shallow first dip can be a vowel's strong upper harmonic lining up (ee / i: the false dip sits ~10 % short
            // of the real period = the note read 1-2 semitones sharp). Look a little further: a clearly deeper dip wins
            if (d[(size_t) c] > 0.06f)
            {
                const int far = std::min (lim, (int) ((float) c * 1.3f) + 2);
                for (int t2 = tau + 1; t2 < far; ++t2) diff (t2);
                int best = c;
                for (int t2 = c + 1; t2 + 1 < far; ++t2)
                    if (d[(size_t) t2] < d[(size_t) best] && d[(size_t) t2] <= d[(size_t) t2 - 1] && d[(size_t) t2] <= d[(size_t) t2 + 1]) best = t2;
                if (d[(size_t) best] < 0.5f * d[(size_t) c]) { c = best; tau = best + 1; }
            }
            const float pa = d[(size_t) c - 1], pb = d[(size_t) c], pc = d[(size_t) tau];
            const float den = pa + pc - 2 * pb;
            const float t = (float) c + (std::abs (den) > 1e-9f ? 0.5f * (pa - pc) / den : 0.0f);
            if (aperiodic != nullptr) *aperiodic = pb;
            return (float) sr / t;
        }
    }
    if (fallback > thresh)
    {
        // breathy / short / mumbled sounds: take the first dip close to the best one (avoids picking an octave too low)
        const int hi = std::min (maxTau - 1, (int) d.size() - 1);
        float best = 1.0f;
        for (int tau = minTau; tau < hi; ++tau) best = std::min (best, d[(size_t) tau]);
        if (best < fallback)
            for (int tau = minTau; tau < hi; ++tau)
                if (d[(size_t) tau] <= best * 1.15f + 0.02f)
                {
                    while (tau + 1 < hi && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                    const float a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
                    const float den = a + c - 2 * b;
                    const float t = (float) tau + (std::abs (den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f);
                    if (rough != nullptr) *rough = true;
                    if (aperiodic != nullptr) *aperiodic = b;
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
            { "retune", "Retune Speed (0 = fastest)", 0, 1, 0.25f }, { "humanize", "Humanize", 0, 1, 0.2f },
            { "key", "Key", -1, 11, -1, { "Project", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },
            { "scale", "Scale", -1, 15, -1, { "Project", "Major", "Minor", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian",
                                               "Locrian", "Major Pent", "Minor Pent", "Blues", "Hungarian Minor", "Phrygian Dom", "Whole Tone", "Chromatic" } }, { "natvib", "Natural Vibrato %", 0, 200, 0 },
            { "mix", "Mix", 0, 1, 1 },
            { "pitch", "Pitch", -12, 12, 0 }, { "detune", "Detune (cents)", -100, 100, 0 },
            { "tuning", "Tuning A4 (Hz)", 400, 500, 440 },
            { "vibDepth", "Add Vibrato (cents)", 0, 100, 0 }, { "vibRate", "Vibrato Rate (Hz)", 3, 9, 5.5f },
            { "snap", "Retune (0 to -3 = harder snap)", -3, 0, 0 },
            { "body", "Body", 0, 1, 0.0f }, { "character", "Character", 0, 1, 0.0f } };   // off by default: just the tune, no tone change
        return ps;
    }
    int latencySamples() const override { return 0; }
    void prepare (double s, int) override
    {
        sr = s;
        shifter.prepare (s); psola.prepare (s); period = (float) (0.008 * s);
        dsFactor = s > 60000 ? 4 : 2;
        analysis.assign (2048, 0.0f); aw = 0; hopCount = 0;
        for (auto& f : tone) f.clear();
        dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f;
        currentRatio = 1.0f; targetRatio = 1.0f; heldNote = 0; missHops = 0; octSlip = 0; lastMidi = 0; centre = 0; slope = 0; jumpHops = 0; tailHops = 0; releasing = false; consonant = false; vibPhase = 0; voiceLvl = 0; pendingMidi = 0; histN = 0; wetTarget = wetG = wetS1 = 0; detectedMidi.store (0); correctedMidi.store (0);
    }
    float detected() const { return detectedMidi.load(); }
    float corrected() const { return correctedMidi.load(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        const int style = (int) p (0);
        float speed = 1.0f - p (1);                            // Retune knob, Antares way: 0 = fastest, 1 = slowest
        const float snap = juce::jlimit (0.0f, 3.0f, -p (12));   // MetaTune-style negative retune: 0 = instant, -3 = hardest step
        const bool hard = style == 3 || snap > 0.0f;
        if (hard) speed = 1.0f;                                // hard tune: instant
        const int root = p (3) < 0 ? ctx.keyRoot : (int) p (3) - 0;
        const int scale = p (4) < 0 ? ctx.scaleIndex : (int) p (4);
        const float mix = p (6);
        const double dsr = sr / dsFactor;
        const float slow = (1.0f - speed) * (1.0f - speed);
        const float timeMs = style == 0 ? 1.0f + 200.0f * slow : 1.0f - 0.3f * snap + 32.0f * slow + 300.0f * slow * slow;   // retune glide: 0.75 = ~4 ms (Antares locks a note within ~3 ms), 0 = ~330 ms
        const float ref = juce::jlimit (400.0f, 500.0f, p (9) > 0 ? p (9) : 440.0f);
        const float coef = std::exp (-1.0f / (0.001f * timeMs * (float) sr));
        const float humanize = hard ? 0.0f : p (2);
        const float offset = std::pow (2.0f, (std::round (p (7)) + p (8) / 100.0f) / 12.0f);   // Pitch (semitones) + Detune (cents)
        const float natVib = p (5) / 100.0f;                    // 0 = vibrato killed (dead flat), 1 = as sung, 2 = enhanced
        const float vibDepth = p (10) / 1200.0f, vibInc = juce::MathConstants<float>::twoPi * p (11) / (float) sr;
        // two smoothing stages = an eased S-curve blend between the untouched consonant and the tuned vowel (~12 ms),
        // so consonants stay crisp but the hand-off isn't a sudden switch (that was the punchy, attacky edge)
        const float wetCoef = 1.0f - std::exp (-1.0f / (0.003f * (float) sr));
        const float wetRel = 1.0f - std::exp (-1.0f / (0.035f * (float) sr));   // the tune lets go of a note slowly (sustain + release, not a drop)
        // Body + Character: chest weight, a little forward bite and soft even-order harmonics on the tuned voice,
        // so it sounds full and alive instead of thin (all zero latency)
        const float body = p (13), chr = p (14);
        const bool colourOn = body > 0.001f || chr > 0.001f;
        if (colourOn)
        {
            tone[0].set (Biquad::LowShelf, sr, 190.0, 0.7, 3.5 * body);
            tone[1].set (Biquad::Peak, sr, 420.0, 0.8, 1.5 * body);
            tone[2].set (Biquad::Peak, sr, 2900.0, 0.9, 2.0 * chr);
        }
        const float drive = 1.0f + 2.5f * chr, bias = 0.25f * chr, tb = std::tanh (bias), satMix = 0.55f * chr;
        auto colour = [&] (float x, int ch) {
            x = tone[0].tick (x, ch); x = tone[1].tick (x, ch); x = tone[2].tick (x, ch);
            if (satMix > 0.0f)
            {
                const float y = (std::tanh (drive * x + bias) - tb) / drive;
                const float hp = y - dcX[ch] + 0.9995f * dcY[ch]; dcX[ch] = y; dcY[ch] = hp;
                x += (hp - x) * satMix;
            }
            return x;
        };
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (l[i] + r[i]);
            if ((dsCount++ % dsFactor) == 0)
            {
                analysis[(size_t) aw] = mono; aw = (aw + 1) % (int) analysis.size();
                const int hop = hard ? (snap >= 2.0f ? 32 : snap >= 1.0f ? 48 : 64) : 128;   // Hard Tune (Antares retune 0): track faster
                if (++hopCount >= hop)
                {
                    hopCount = 0;
                    // one full-range detector for every register: the window follows the voice (~3 cycles of the sung
                    // note = low lag, so vibrato isn't smeared into a wave); at a word start or on a miss try short, then long
                    const float th = style == 0 ? 0.12f : 0.18f, fbk = style == 0 ? 0.3f : 0.45f;
                    bool rough = false; int wn = 512; float hz = 0, aper = 1;
                    auto detect = [&] (int len) {
                        len = juce::jlimit (256, 1024, len);
                        std::array<float, 1024> win {};
                        for (int k = 0; k < len; ++k) win[(size_t) k] = analysis[(size_t) ((aw - len + k + (int) analysis.size()) % (int) analysis.size())];
                        wn = len;
                        return detectPitchYin (win.data(), len, dsr, th, fbk, 55.0f, 1100.0f, &rough, &aper);
                    };
                    if (lastMidi > 0) hz = detect ((int) ((3.2 - 0.3 * snap) * dsr / (ref * std::pow (2.0, (lastMidi - 69.0) / 12.0))));
                    if (hz <= 0) hz = detect (512);
                    if (hz <= 0) hz = detect (1024);
                    float midi = hz > 0 ? 69.0f + 12.0f * std::log2 (hz / ref) : 0.0f;
                    float lvl = 0;
                    for (int k = 0; k < wn; ++k) { const float a = analysis[(size_t) ((aw - wn + k + (int) analysis.size()) % (int) analysis.size())]; lvl += a * a; }
                    lvl = std::sqrt (lvl / (float) wn);
                    // breaths, room noise and hiss: too quiet next to the voice, or not clearly pitched - leave them untuned
                    const float hopSec = (float) hop / (float) dsr;
                    voiceLvl *= std::exp (-hopSec / 0.5f);
                    const float tailMidi = midi;   // the reading before the gate: a quiet / breathy tail still says where the voice is
                    if (lvl < (hard ? 0.07f : 0.1f) * voiceLvl || lvl < 2.0e-4f || aper > 0.35f) midi = 0.0f;   // Hard Tune: quieter note tails still count as the note   // quiet note tails still count as the note
                    // a rough (noisy) reading far from the note being sung is breath/consonant noise, not a new note
                    if (rough && lastMidi > 0 && std::abs (midi - lastMidi) > 1.5f) midi = 0.0f;
                    // a rough reading inside a held note keeps the tune locked as-is (noise must not bend the target)
                    if (rough && midi > 0 && heldNote > 0) { missHops = 0; roughMidi = midi; midi = -1.0f; }
                    // the voice coming back after a gap on another note is a fresh start, not a slip of the old note
                    if (midi > 0 && lastMidi > 0 && missHops >= 2 && std::abs (midi - lastMidi) > 1.0f) { heldNote = 0; lastMidi = 0; octSlip = 0; slope = 0; jumpHops = 0; }
                    if (midi > 0 && lastMidi > 0)
                    {
                        // octave error guard: a sudden exact octave jump inside a note is a detector slip - fold it back
                        const float dj = midi - lastMidi;
                        if (std::abs (dj - 12.0f) < 0.7f && octSlip < 8) { midi -= 12.0f; ++octSlip; }
                        else if (std::abs (dj + 12.0f) < 0.7f && octSlip < 8) { midi += 12.0f; ++octSlip; }
                        else octSlip = 0;
                    }
                    // inside a held note the reading is the median of the last 3: vowel / formant changes jolt the detector
                    // for a hop, and that jolt went straight into the tuned note (the "swingy" wobble instead of a straight lock)
                    // (the median only steers which note is chosen; the correction itself still follows the voice exactly)
                    float med = midi;
                    if (midi > 0 && lastMidi > 0 && std::abs (midi - lastMidi) < 1.0f)
                    {
                        hist[(size_t) (histN++ % 3)] = midi;
                        if (histN >= 3) med = std::max (std::min (hist[0], hist[1]), std::min (std::max (hist[0], hist[1]), hist[2]));
                    }
                    else if (midi > 0) { hist[0] = midi; histN = 1; }
                    // a new note (word start or a leap) must read the same twice in a row before it's tuned to: one-off
                    // misreads from noise or consonants were the chirps / squeaks (a wrong ratio for a few ms)
                    if (midi > 0 && (lastMidi == 0 || std::abs (midi - lastMidi) > 1.0f))
                    {
                        const bool same = pendingMidi > 0 && std::abs (midi - pendingMidi) < (rough || pendingRough ? 0.35f : 0.6f);
                        if (same) pendingMidi = 0;
                        else { pendingMidi = midi; pendingRough = rough; midi = 0.0f; }   // counts as a miss: noise can't hold a note forever
                    }
                    else if (midi != -1.0f) pendingMidi = 0;
                    // only the clearly sung (vowel) part is tuned: consonants, breaths, rough edges and word starts pass
                    // through untouched, so enunciation and punch survive (shifting them was the plastic, mashed sound)
                    const bool wasWet = wetTarget > 0.5f;
                    wetTarget = midi > 0 && (hard || ! rough || (style != 0 && heldNote > 0)) && aper < (hard ? (wetTarget > 0.5f ? 0.45f : 0.35f) : style == 0 ? (wetTarget > 0.5f ? 0.3f : 0.2f) : (wetTarget > 0.5f ? 0.45f : 0.28f)) ? 1.0f : 0.0f;   // Antares-style: once a note is held, breathy / raspy voicing stays tuned too   // Hard Tune tunes the rough edges too: no natural voice
                    // Hard Tune (Antares-style): a rough patch inside a held note stays fully locked to the note, so the
                    // natural pitch never leaks back in mid-word (that leak was the softer, Waves-like snap)
                    if (midi == -1.0f && heldNote > 0 && (hard || (style != 0 && wasWet && std::abs (roughMidi - (float) heldNote) < 1.0f))) wetTarget = 1.0f;   // rasp / breath on the held note stays tuned (Antares tunes it too)   // other styles: a breathy stretch right on the held note stays tuned for the sustain time
                    consonant = rough || aper > 0.35f;   // consonants / noise leave the tune at once, so they stay the singer's
                    // Hard Tune sustain (Auto-Tune feel): the fading / breathy tail of a held note stays on the note
                    // for ~70 ms instead of slipping back to the natural pitch, so the corrected note rings on
                    if (midi > 0) tailHops = 0;
                    else if (midi == 0.0f && heldNote > 0 && lvl > 1.0e-4f && (hard || ! consonant) && ++tailHops * hopSec < (hard ? 0.28f : 0.16f))
                    {
                        wetTarget = 1.0f;
                        // the tail keeps following the voice onto the held note (a frozen ratio drifted off as the voice sagged)
                        if (tailMidi > 0 && std::abs (tailMidi - (float) heldNote) < 1.0f)
                            targetRatio = std::pow (2.0f, juce::jlimit (-1.5f, 1.5f, (float) heldNote - tailMidi) / 12.0f);
                    }
                    if (midi > 0)
                    {
                        const bool onset = heldNote == 0;
                        voiceLvl = std::max (voiceLvl, lvl);
                        // the analysis window lags the voice by half its length: extrapolate the pitch slope so vibrato
                        // and inflections are corrected where the voice is now (this lag was the leftover "wave")
                        slope = onset || std::abs (midi - lastMidi) > 1.0f ? 0.0f : 0.6f * slope + 0.4f * (midi - lastMidi);
                        const float now = midi + juce::jlimit (-0.5f, 0.5f, slope * 0.5f * (float) wn / (float) hop);
                        missHops = 0; lastMidi = midi; releasing = false;
                        period = (float) (sr / hz);
                        // the note is chosen from the sung note's centre (vibrato removed), so vibrato never flips notes
                        const float steer = med + (now - midi);
                        centre = onset ? now : centre + (steer - centre) * (1.0f - std::exp (-hopSec / 0.12f));
                        int target = snapToScale ((int) std::round (centre), root, scale);
                        if (heldNote > 0 && target != heldNote && std::abs (centre - (float) heldNote) < (hard ? 0.55f : 0.8f) && inScale (heldNote, root, scale)) target = heldNote;
                        // a real jump to another note is taken within ~20 ms
                        const int fast = snapToScale ((int) std::round (now), root, scale);
                        if (heldNote > 0 && fast != heldNote && std::abs (now - (float) heldNote) > 0.75f)
                        {
                            if (++jumpHops * hopSec >= (hard ? 0.012f - 0.002f * snap : 0.02f)) { target = fast; centre = now; jumpHops = 0; }   // Hard Tune steps sooner
                        }
                        else jumpHops = 0;
                        // the note being sung always leads: a held note more than a semitone from the voice (a fast drop or
                        // leap the hold hasn't caught yet) is never forced on it - that big shift was the transposed / formanted tone
                        if (std::abs ((float) target - centre) > 1.0f) { target = snapToScale ((int) std::round (centre), root, scale); jumpHops = 0; }
                        heldNote = target;
                        float semis = juce::jlimit (-1.5f, 1.5f, (float) target - now + natVib * (now - centre));
                        if (style == 0 && std::abs (semis) < humanize * 0.25f) semis *= (1.0f - humanize);   // Natural only: let tiny drift through
                        targetRatio = std::pow (2.0f, semis / 12.0f);
                        if (onset && (hard || speed >= 0.5f)) currentRatio = targetRatio;   // new word starts already on the note
                        detectedMidi.store (midi); correctedMidi.store ((float) target);
                    }
                    // a rough reading close to the held note still steers the correction (half way per hop), so the voice
                    // drifting under rasp is pulled back onto the key note instead of riding a frozen ratio off it
                    else if (midi == -1.0f && heldNote > 0 && std::abs (roughMidi - (float) heldNote) < 1.0f && wetTarget > 0.5f)
                    {
                        const float want = juce::jlimit (-1.5f, 1.5f, (float) heldNote - roughMidi), cur = 12.0f * std::log2 (targetRatio);
                        targetRatio = std::pow (2.0f, (cur + 0.5f * (want - cur)) / 12.0f);
                    }
                    // keep the tune locked through short dropouts (breaths, consonants, mumbles) - only let go after ~80 ms
                    else if (midi == 0.0f && ++missHops > (int) ((hard ? 0.32 : 0.22) * dsr / hop)) { releasing = true; heldNote = 0; lastMidi = 0; octSlip = 0; slope = 0; jumpHops = 0; }
                }
            }
            currentRatio = targetRatio + (currentRatio - targetRatio) * coef;
            float ratio = currentRatio * offset;
            if (vibDepth > 0 && heldNote > 0) { ratio *= std::exp2 (vibDepth * std::sin (vibPhase)); vibPhase = std::fmod (vibPhase + vibInc, juce::MathConstants<float>::twoPi); }
            wetS1 += (wetTarget - wetS1) * (wetTarget > wetS1 || consonant ? wetCoef : wetRel);
            wetG += (wetS1 - wetG) * (wetS1 > wetG || consonant ? wetCoef : wetRel);
            if (releasing && wetG < 0.01f) { targetRatio = currentRatio = 1.0f; releasing = false; }   // the note fades out still on pitch
            float wl = l[i], wr = r[i], pl = l[i], pr = r[i];
            // Hard Tune keeps its own snap; every gliding style (Retune / Humanize) keeps the voice's tone
            // only the shifter in use runs (half the CPU)
            if (style == 3) shifter.process (wl, wr, 1.0f + (ratio - 1.0f) * wetG, period);   // unvoiced: the head stops moving = untouched
            else
            {
                psola.process (pl, pr, ratio, period);
                l[i] = psola.dryL; r[i] = psola.dryR;
                wl = l[i] + wetG * (pl - l[i]); wr = r[i] + wetG * (pr - r[i]);
            }
            if (colourOn) { const float cl = colour (wl, 0), cr = colour (wr, 1); wl += wetG * (cl - wl); wr += wetG * (cr - wr); }   // sung vowels only: consonants stay crisp
            l[i] = l[i] * (1 - mix) + wl * mix;
            r[i] = r[i] * (1 - mix) + wr * mix;
        }
    }
private:
    double sr = 44100; int dsFactor = 2, dsCount = 0, aw = 0, hopCount = 0;
    std::vector<float> analysis; SpliceShifter shifter; PsolaShifter psola;
    float currentRatio = 1, targetRatio = 1, roughMidi = 0;
    std::array<float, 3> hist {}; int histN = 0;
    int heldNote = 0, missHops = 0, octSlip = 0, jumpHops = 0, tailHops = 0; float voiceLvl = 0, pendingMidi = 0; bool pendingRough = false, releasing = false, consonant = false;
    float lastMidi = 0, centre = 0, slope = 0, vibPhase = 0, wetTarget = 0, wetG = 0, wetS1 = 0;
    float period = 350;
    Biquad tone[3]; float dcX[2] {}, dcY[2] {};
    std::atomic<float> detectedMidi { 0 }, correctedMidi { 0 };
};

// ------------------------------------------------------------ Key Finder
juce::String KeyGuess::describe() const
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    juce::String c = (cents >= 0 ? "+" : "") + juce::String (juce::roundToInt (cents)) + " cents";
    return juce::String (names[(root % 12 + 12) % 12]) + (scaleIndex == 1 ? " minor" : " major")
         + "   " + juce::String (juce::roundToInt (bpm)) + " BPM   " + c;
}

std::vector<double> detectOnsets (const float* x, int n, double sr, double minGap)
{
    std::vector<double> out;
    const int hop = 128;
    if (x == nullptr || n < hop * 4 || sr <= 0) return out;
    const int hops = n / hop;
    std::vector<float> db ((size_t) hops);
    for (int h = 0; h < hops; ++h)
    {
        double e = 0;
        for (int i = 0; i < hop; ++i) { const float v = x[h * hop + i]; e += v * v; }
        db[(size_t) h] = (float) (10.0 * std::log10 (e / hop + 1e-12));
    }
    double last = -1e9;
    for (int h = 1; h < hops; ++h)
    {
        float avg = 0; int k = 0;
        for (int j = std::max (0, h - 4); j < h; ++j) { avg += db[(size_t) j]; ++k; }
        avg /= (float) k;
        const double t = h * hop / sr;
        if (db[(size_t) h] > -45.0f && db[(size_t) h] - avg > 9.0f && t - last >= minGap)
        {
            out.push_back (std::max (0.0, t - 0.003));
            last = t;
        }
    }
    return out;
}

KeyGuess findKey (const float* x, int n, double sr)
{
    KeyGuess g;
    constexpr int order = 13, N = 1 << order;
    if (x == nullptr || n < N) return g;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2), win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (N - 1));
    struct Pk { float midi, w; };
    std::vector<Pk> peaks;
    const int kLo = (int) std::ceil (55.0 * N / sr), kHi = juce::jmin (N / 2 - 2, (int) (5000.0 * N / sr));
    for (int at = 0; at + N <= n; at += N / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = x[at + i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data());
        float mx = 0; for (int k = kLo; k <= kHi; ++k) mx = std::max (mx, buf[(size_t) k]);
        if (mx < 1.0e-3f) continue;
        for (int k = juce::jmax (kLo, 2); k <= kHi; ++k)
        {
            const float m = buf[(size_t) k];
            if (m < 0.05f * mx || m <= buf[(size_t) k - 1] || m < buf[(size_t) k + 1]) continue;
            const float a = std::log (buf[(size_t) k - 1] + 1e-9f), b = std::log (m + 1e-9f), c = std::log (buf[(size_t) k + 1] + 1e-9f);
            const float den = a - 2 * b + c;
            const float d = std::abs (den) > 1e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
            const double hz = (k + d) * sr / N;
            peaks.push_back ({ (float) (69.0 + 12.0 * std::log2 (hz / 440.0)), m / mx });
        }
    }
    if (peaks.empty()) return g;
    // tuning: weighted circular mean of how far the partials sit from equal temperament (above ~250 Hz, where bins are fine)
    double sx = 0, sy = 0;
    for (auto& p : peaks)
        if (p.midi > 59.0f)
        {
            const double dev = p.midi - std::round (p.midi), ang = juce::MathConstants<double>::twoPi * dev;
            sx += p.w * std::cos (ang); sy += p.w * std::sin (ang);
        }
    g.cents = (float) (std::atan2 (sy, sx) / juce::MathConstants<double>::twoPi * 100.0);
    // key: chroma against the Krumhansl major / minor profiles
    double chroma[12] {};
    for (auto& p : peaks)
    {
        const int pc = (((int) std::lround (p.midi - g.cents / 100.0f)) % 12 + 12) % 12;
        chroma[pc] += std::sqrt (p.w);
    }
    static const double maj[12] { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[12] { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto corr = [&] (const double* prof, int root) {
        double mx = 0, my = 0; for (int i = 0; i < 12; ++i) { mx += chroma[(i + root) % 12]; my += prof[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) { const double a = chroma[(i + root) % 12] - mx, b = prof[i] - my; sxy += a * b; sxx += a * a; syy += b * b; }
        return sxx > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };
    double best = -2, second = -2;
    for (int mode = 0; mode < 2; ++mode)
        for (int r = 0; r < 12; ++r)
        {
            const double c = corr (mode == 0 ? maj : mnr, r);
            if (c > best) { second = best; best = c; g.root = r; g.scaleIndex = mode; }
            else if (c > second) second = c;
        }
    g.confidence = (float) juce::jlimit (0.0, 1.0, (best - second) * 5.0);
    // tempo: spectral-flux onsets, autocorrelation scored over 4 beat multiples
    constexpr int o2 = 10, N2 = 1 << o2, hop = 512;
    juce::dsp::FFT f2 (o2);
    std::vector<float> b2 ((size_t) N2 * 2), prev ((size_t) N2 / 2, 0.0f), flux;
    for (int at = 0; at + N2 <= n; at += hop)
    {
        std::fill (b2.begin(), b2.end(), 0.0f);
        for (int i = 0; i < N2; ++i) b2[(size_t) i] = x[at + i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (N2 - 1)));
        f2.performFrequencyOnlyForwardTransform (b2.data());
        float fl = 0;
        for (int k = 1; k < N2 / 2; ++k) { const float m = std::log1p (100.0f * b2[(size_t) k]); fl += std::max (0.0f, m - prev[(size_t) k]); prev[(size_t) k] = m; }
        flux.push_back (fl);
    }
    const double fr = sr / hop;
    const int F = (int) flux.size();
    if (F > (int) (fr * 4))
    {
        std::vector<double> on ((size_t) F), pre ((size_t) F + 1, 0.0);
        for (int i = 0; i < F; ++i) pre[(size_t) i + 1] = pre[(size_t) i] + flux[(size_t) i];
        const int w = juce::jmax (2, (int) (0.25 * fr / 2));
        for (int i = 0; i < F; ++i)
        {
            const int a = juce::jmax (0, i - w), b = juce::jmin (F, i + w + 1);
            on[(size_t) i] = std::max (0.0, flux[(size_t) i] - (pre[(size_t) b] - pre[(size_t) a]) / (b - a));
        }
        const int maxLag = juce::jmin (F - 1, (int) (fr * 60.0 / 70.0 * 4) + 2);
        std::vector<double> ac ((size_t) maxLag + 1, 0.0);
        for (int L = 1; L <= maxLag; ++L) for (int i = L; i < F; ++i) ac[(size_t) L] += on[(size_t) i] * on[(size_t) i - L];
        auto at = [&] (double lag) { const int i = (int) lag; const double f = lag - i; return i + 1 > maxLag ? 0.0 : ac[(size_t) i] * (1 - f) + ac[(size_t) i + 1] * f; };
        auto score = [&] (double bpm) { const double lag = fr * 60.0 / bpm; double sc = 0; for (int h = 1; h <= 4; ++h) sc += at (lag * h); return sc; };
        double bestS = -1;
        for (double bpm = 70.0; bpm <= 180.0; bpm += 0.1)
            if (const double sc = score (bpm); sc > bestS) { bestS = sc; g.bpm = bpm; }
        // half-time reads (71 for a 142 beat): take the double tempo when it is nearly as strong (FL / trap convention)
        if (g.bpm < 90.0)
        {
            double b2 = g.bpm * 2, s2 = -1;
            for (double bpm = g.bpm * 2 - 1.0; bpm <= g.bpm * 2 + 1.0; bpm += 0.1) if (const double sc = score (bpm); sc > s2) { s2 = sc; b2 = bpm; }
            if (s2 > 0.6 * bestS) g.bpm = b2;
        }
        g.bpm = std::round (g.bpm);
    }
    return g;
}

// ------------------------------------------------------------ registry
std::unique_ptr<BuiltinEffect> createStudioEffect (const juce::String& id);
std::unique_ptr<BuiltinEffect> createPurify();

ScopeTap& masterScope() { static ScopeTap t; return t; }

const std::vector<EffectInfo>& builtinEffects()
{
    static const std::vector<EffectInfo> fx {
        { "strip", "Channel Strip", "Studio" }, { "steq7", "Studio EQ (12 band)", "Studio" }, { "stdyn", "Studio Dyn", "Studio" },
        { "fet76", "FET 76", "Studio" }, { "stgate", "Studio Gate/Expander", "Studio" }, { "stlimit", "Studio Limiter", "Studio" }, { "stmax", "Maximizer (multiband)", "Studio" },
        { "hhmaster", "Hum House Master", "Mastering" }, { "purify", "Purify (one knob)", "Mastering" }, { "satur", "Saturator", "Studio" }, { "stvocal", "Studio Vocal", "Studio" },
        { "autopitch", "AutoPitch", "Vocal" },
        { "vroom", "HumHouse Vocal Room", "Vocal" },
        { "deess", "De-Esser", "Vocal" },
        { "eq", "Para EQ", "EQ & Filter" }, { "filter", "Sweep Filter", "EQ & Filter" },
        { "comp", "Compressor", "Dynamics" }, { "limiter", "Limiter", "Dynamics" }, { "gate", "Noise Gate", "Dynamics" },
        { "pump", "Pump (Sidechain)", "Dynamics" }, { "clip", "Soft Clipper", "Dynamics" },
        { "verb", "Room Verb", "Space" }, { "delay", "Echo Tape", "Space" }, { "chorus", "Chorus", "Modulation" }, { "flanger", "Flanger", "Modulation" }, { "flangus", "Flangus", "Modulation" },
        { "phaser", "Phase Swirl", "Modulation" }, { "wide", "Stereo Wide", "Utility" }, { "gain", "Gain / Pan", "Utility" },
        { "grit", "Grit", "Distortion" }, { "crush", "Bit Crush", "Distortion" },
        { "glitch", "Glitch Gate", "Time FX" }, { "scope", "Scope (wave / spectrum)", "Utility" } };
    return fx;
}

std::unique_ptr<BuiltinEffect> createBuiltinEffect (const juce::String& id)
{
    if (auto s = createStudioEffect (id)) return s;
    if (id == "purify") { auto f = createPurify(); f->initDefaults(); return f; }
    if (id == "scope") return std::make_unique<ScopeFx>();
    if (id == "eq") return std::make_unique<ParaEQ>();
    if (id == "comp") return std::make_unique<Compressor>();
    if (id == "limiter") return std::make_unique<Limiter>();
    if (id == "verb") return std::make_unique<RoomVerb>();
    if (id == "delay") return std::make_unique<EchoTape>();
    if (id == "chorus") return std::make_unique<Chorus>();
    if (id == "flanger") return std::make_unique<Flanger>();
    if (id == "flangus") return std::make_unique<Flangus>();
    if (id == "phaser") return std::make_unique<PhaseSwirl>();
    if (id == "grit") return std::make_unique<Grit>();
    if (id == "filter") return std::make_unique<SweepFilter>();
    if (id == "glitch") return std::make_unique<GlitchGate>();
    if (id == "hhmaster") return std::make_unique<HumHouseMaster>();
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

void migrateFxParams (const juce::String& type, std::map<juce::String, float>& params)
{
    // AutoPitch Retune used to be "speed" with 1 = fastest; it is now "retune" with 0 = fastest (Antares way)
    if (type != "autopitch") return;
    auto it = params.find ("speed");
    if (it == params.end()) return;
    if (params.count ("retune") == 0) params["retune"] = juce::jlimit (0.0f, 1.0f, 1.0f - it->second);
    params.erase (it);
}
}
