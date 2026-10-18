// Studio-grade console set: own DSP modelled on classic stock-plugin workflows.
#include "Effects.h"
#include <cmath>
#include <complex>

namespace hhs
{
static inline float dB (float d) { return std::pow (10.0f, d / 20.0f); }
static inline float toDb (float g) { return 20.0f * std::log10 (std::max (g, 1.0e-7f)); }

// shared feed-forward compressor core (soft knee, peak/RMS, sidechain HPF)
struct CompCore
{
    double sr = 44100; float env = 0, rms = 0; Biquad sc;
    float gr = 0;
    void prepare (double s) { sr = s; env = rms = 0; sc.clear(); }
    float gainDb (float detectDb, float th, float ratio, float knee)
    {
        const float over = detectDb - th;
        if (2 * over < -knee) return 0;
        if (knee > 0 && 2 * std::abs (over) <= knee) return (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
        return over * (1 / ratio - 1);
    }
    void run (juce::AudioBuffer<float>& b, int n, float th, float ratio, float knee, float attMs, float relMs, bool useRms, float scHpf, float makeup, float mix)
    {
        sc.set (Biquad::HighPass, sr, std::max (20.0f, scHpf), 0.707, 0);
        const float att = std::exp (-1.0f / (0.001f * attMs * (float) sr)), rel = std::exp (-1.0f / (0.001f * relMs * (float) sr));
        const float rc = std::exp (-1.0f / (0.01f * (float) sr)), mg = dB (makeup);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        float maxGr = 0;
        for (int i = 0; i < n; ++i)
        {
            float dl = l[i], dr = r[i];
            if (scHpf > 21) { dl = sc.tick (dl, 0); dr = sc.tick (dr, 1); }
            float a = std::max (std::abs (dl), std::abs (dr));
            if (useRms) { rms = rc * rms + (1 - rc) * a * a; a = std::sqrt (rms) * 1.414f; }
            const float target = gainDb (toDb (a), th, ratio, knee);
            env = target < env ? att * env + (1 - att) * target : rel * env + (1 - rel) * target;
            maxGr = std::min (maxGr, env);
            const float g = dB (env) * mg;
            l[i] = l[i] * (1 - mix) + l[i] * g * mix; r[i] = r[i] * (1 - mix) + r[i] * g * mix;
        }
        gr = maxGr;
    }
};

// EQ III style 7-band. Natural = zero-latency IIR; Linear = linear-phase FIR of the same curve (adds firN/2 latency).
// Every band can be dynamic: it moves by Range when its own band gets louder than Threshold (zero latency).
class StudioEQ7 : public BuiltinEffect
{
public:
    static constexpr int firOrder = 11, firN = 1 << firOrder, numBands = 6, phaseIdx = 21, dynBase = 22;
    StudioEQ7() { initDefaults(); }
    const char* typeId() const override { return "steq7"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "hpf", "HPF", 20, 1000, 20 }, { "hpfSlope", "HPF Slope", 0, 3, 1, { "6 dB", "12 dB", "18 dB", "24 dB" } },
            { "lsG", "Low Shelf", -18, 18, 0 }, { "lsF", "LS Freq", 20, 1000, 100 },
            { "b1G", "LMF Gain", -18, 18, 0 }, { "b1F", "LMF Freq", 40, 2000, 250 }, { "b1Q", "LMF Q", 0.1f, 10, 1 },
            { "b2G", "MF Gain", -18, 18, 0 }, { "b2F", "MF Freq", 200, 8000, 1000 }, { "b2Q", "MF Q", 0.1f, 10, 1 },
            { "b3G", "HMF Gain", -18, 18, 0 }, { "b3F", "HMF Freq", 1000, 16000, 3500 }, { "b3Q", "HMF Q", 0.1f, 10, 1 },
            { "b4G", "Presence", -18, 18, 0 }, { "b4F", "Pres Freq", 2000, 18000, 6000 }, { "b4Q", "Pres Q", 0.1f, 10, 1 },
            { "hsG", "High Shelf", -18, 18, 0 }, { "hsF", "HS Freq", 1500, 20000, 10000 },
            { "lpf", "LPF", 1000, 22000, 22000 }, { "lpfSlope", "LPF Slope", 0, 3, 1, { "6 dB", "12 dB", "18 dB", "24 dB" } },
            { "out", "Output", -18, 18, 0 },
            { "phase", "Phase", 0, 1, 0, { "Natural (0 latency)", "Linear (~23 ms)" } },
            { "lsDyn", "LS Dynamic", 0, 1, 0, { "Off", "On" } }, { "lsDTh", "LS Dyn Thresh", -60, 0, -24 }, { "lsDRg", "LS Dyn Range", -18, 18, -6 },
            { "lsDAt", "LS Dyn Attack", 0.1f, 100, 5 }, { "lsDRl", "LS Dyn Release", 5, 1000, 120 },
            { "b1Dyn", "LMF Dynamic", 0, 1, 0, { "Off", "On" } }, { "b1DTh", "LMF Dyn Thresh", -60, 0, -24 }, { "b1DRg", "LMF Dyn Range", -18, 18, -6 },
            { "b1DAt", "LMF Dyn Attack", 0.1f, 100, 5 }, { "b1DRl", "LMF Dyn Release", 5, 1000, 120 },
            { "b2Dyn", "MF Dynamic", 0, 1, 0, { "Off", "On" } }, { "b2DTh", "MF Dyn Thresh", -60, 0, -24 }, { "b2DRg", "MF Dyn Range", -18, 18, -6 },
            { "b2DAt", "MF Dyn Attack", 0.1f, 100, 5 }, { "b2DRl", "MF Dyn Release", 5, 1000, 120 },
            { "b3Dyn", "HMF Dynamic", 0, 1, 0, { "Off", "On" } }, { "b3DTh", "HMF Dyn Thresh", -60, 0, -24 }, { "b3DRg", "HMF Dyn Range", -18, 18, -6 },
            { "b3DAt", "HMF Dyn Attack", 0.1f, 100, 5 }, { "b3DRl", "HMF Dyn Release", 5, 1000, 120 },
            { "b4Dyn", "Pres Dynamic", 0, 1, 0, { "Off", "On" } }, { "b4DTh", "Pres Dyn Thresh", -60, 0, -24 }, { "b4DRg", "Pres Dyn Range", -18, 18, -6 },
            { "b4DAt", "Pres Dyn Attack", 0.1f, 100, 5 }, { "b4DRl", "Pres Dyn Release", 5, 1000, 120 },
            { "hsDyn", "HS Dynamic", 0, 1, 0, { "Off", "On" } }, { "hsDTh", "HS Dyn Thresh", -60, 0, -24 }, { "hsDRg", "HS Dyn Range", -18, 18, -6 },
            { "hsDAt", "HS Dyn Attack", 0.1f, 100, 5 }, { "hsDRl", "HS Dyn Release", 5, 1000, 120 } };
        return ps;
    }
    int latencySamples() const override { return p (phaseIdx) > 0.5f ? firN / 2 : 0; }
    float meter (int i) const override { return i >= 0 && i < numBands ? dynDb[(size_t) i].load() : 0.0f; }
    bool hasResponse() const override { return true; }
    float responseDb (double hz) override { update(); return toDb ((float) mag (hz)); }
    void prepare (double s, int maxBlock) override
    {
        sr = s; maxN = juce::jmax (1, maxBlock);
        for (auto& f : f_) f.clear();
        for (auto& f : dynF) f.clear();
        for (auto& f : det) f.clear();
        for (auto& e : env) e = -120.0f;
        for (auto& c : cur) c = 0.0f;
        hash = -1;
        if (conv) conv->prepare ({ sr, (juce::uint32) maxN, 2 });
        else if (p (phaseIdx) > 0.5f) makeConv();
        firDirty = true; firHold = 0;
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        update();
        if (p (phaseIdx) > 0.5f)
        {
            if (! conv) makeConv();
            if (firDirty && --firHold <= 0) { loadFir(); firDirty = false; firHold = juce::jmax (1, (int) (0.06 * sr / juce::jmax (1, n))); }
            juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), 2, (size_t) n);
            conv->process (juce::dsp::ProcessContextReplacing<float> (blk));
        }
        else processIir (b, n);
        processDynamic (b, n);
    }
private:
    double sr = 44100; int maxN = 512;
    Biquad f_[10], dynF[numBands], det[numBands];
    float hash = -1, hpC = 0, lpC = 1, hpS[2] {}, lpS[2] {};
    float env[numBands] {}, cur[numBands] {};
    std::array<std::atomic<float>, numBands> dynDb {};
    std::unique_ptr<juce::dsp::Convolution> conv;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fftData;
    bool firDirty = true; int firHold = 0;

    static int gIdx (int k) { static const int g[] { 2, 4, 7, 10, 13, 16 }; return g[k]; }
    float bandF (int k) const { return p (gIdx (k) + 1); }
    float bandQ (int k) const { return k == 0 || k == numBands - 1 ? 0.707f : p (gIdx (k) + 2); }
    Biquad::Type bandType (int k) const { return k == 0 ? Biquad::LowShelf : k == numBands - 1 ? Biquad::HighShelf : Biquad::Peak; }

    void update()
    {
        float h = 0; for (int i = 0; i < 21; ++i) h += p (i) * (float) (i + 3) * 0.917f;
        if (h == hash) return;
        hash = h;
        f_[0].set (Biquad::HighPass, sr, p (0), 0.707, 0); f_[1].set (Biquad::HighPass, sr, p (0), 0.707, 0);
        for (int k = 0; k < numBands; ++k)
        {
            f_[2 + k].set (bandType (k), sr, bandF (k), bandQ (k), p (gIdx (k)));
            det[k].set (k == 0 ? Biquad::LowPass : k == numBands - 1 ? Biquad::HighPass : Biquad::BandPass, sr, bandF (k), bandQ (k), 0);
            dynF[k].set (bandType (k), sr, bandF (k), bandQ (k), cur[k]);
        }
        f_[8].set (Biquad::LowPass, sr, p (18), 0.707, 0); f_[9].set (Biquad::LowPass, sr, p (18), 0.707, 0);
        hpC = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * p (0) / (float) sr);
        lpC = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * p (18) / (float) sr);
        firDirty = true;
    }
    double mag (double hz) const
    {
        using C = std::complex<double>;
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.5, hz) / sr;
        const C z1 = std::polar (1.0, -w), z2 = z1 * z1;
        auto bq = [&] (const Biquad& q) { return (C (q.b0) + C (q.b1) * z1 + C (q.b2) * z2) / (C (1.0) + C (q.a1) * z1 + C (q.a2) * z2); };
        auto lp1 = [&] (float c) { return C (c) / (C (1.0) - C (1.0 - c) * z1); };
        C H (1.0);
        const int hs = (int) p (1), ls = (int) p (19);
        if (p (0) > 21) { if (hs == 0 || hs == 2) H *= C (1.0) - lp1 (hpC); if (hs >= 1) H *= bq (f_[0]); if (hs == 3) H *= bq (f_[1]); }
        for (int k = 0; k < numBands; ++k) if (p (gIdx (k)) != 0) H *= bq (f_[2 + k]);
        if (p (18) < 21000) { if (ls == 0 || ls == 2) H *= lp1 (lpC); if (ls >= 1) H *= bq (f_[8]); if (ls == 3) H *= bq (f_[9]); }
        return std::abs (H) * dB (p (20));
    }
    void makeConv()
    {
        conv = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { 256 });
        conv->prepare ({ sr, (juce::uint32) maxN, 2 });
        fft = std::make_unique<juce::dsp::FFT> (firOrder);
        fftData.assign ((size_t) firN * 2, 0.0f);
        firDirty = true; firHold = 0;
    }
    void loadFir()
    {
        std::fill (fftData.begin(), fftData.end(), 0.0f);
        for (int k = 0; k <= firN / 2; ++k) fftData[(size_t) (2 * k)] = (float) mag (k * sr / firN);
        fft->performRealOnlyInverseTransform (fftData.data());
        juce::AudioBuffer<float> ir (1, firN);
        const double tp = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < firN; ++i)
        {
            const double win = 0.42 - 0.5 * std::cos (tp * i / firN) + 0.08 * std::cos (2.0 * tp * i / firN);
            ir.setSample (0, i, (float) (fftData[(size_t) ((i + firN / 2) % firN)] * win));
        }
        conv->loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }
    void processIir (juce::AudioBuffer<float>& b, int n)
    {
        const int hs = (int) p (1), ls = (int) p (19);
        const bool hp = p (0) > 21, lp = p (18) < 21000;
        const float g = dB (p (20));
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                if (hp)
                {
                    if (hs == 0 || hs == 2) { hpS[ch] += hpC * (x - hpS[ch]); x -= hpS[ch]; }
                    if (hs >= 1) x = f_[0].tick (x, ch);
                    if (hs == 3) x = f_[1].tick (x, ch);
                }
                for (int k = 0; k < numBands; ++k) if (p (gIdx (k)) != 0) x = f_[2 + k].tick (x, ch);
                if (lp)
                {
                    if (ls == 0 || ls == 2) { lpS[ch] += lpC * (x - lpS[ch]); x = lpS[ch]; }
                    if (ls >= 1) x = f_[8].tick (x, ch);
                    if (ls == 3) x = f_[9].tick (x, ch);
                }
                d[i] = x * g;
            }
        }
    }
    void processDynamic (juce::AudioBuffer<float>& b, int n)
    {
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int k = 0; k < numBands; ++k)
        {
            const int d = dynBase + k * 5;
            if (p (d) < 0.5f) { cur[k] = 0; env[k] = -120.0f; dynDb[(size_t) k] = 0.0f; continue; }
            const float th = p (d + 1), range = p (d + 2), attMs = p (d + 3), relMs = p (d + 4);
            for (int i0 = 0; i0 < n; i0 += 32)
            {
                const int len = juce::jmin (32, n - i0);
                float pk = 0;
                for (int i = i0; i < i0 + len; ++i) pk = std::max (pk, std::abs (det[k].tick (0.5f * (l[i] + r[i]), 0)));
                const float lvl = toDb (pk);
                const float ca = std::exp (-(float) len / (0.001f * attMs * (float) sr)), cr = std::exp (-(float) len / (0.001f * relMs * (float) sr));
                env[k] = lvl > env[k] ? ca * env[k] + (1 - ca) * lvl : cr * env[k] + (1 - cr) * lvl;
                const float over = env[k] - th;
                const float off = range * juce::jlimit (0.0f, 1.0f, over / 12.0f);
                if (std::abs (off - cur[k]) > 0.05f) { cur[k] = off; dynF[k].set (bandType (k), sr, bandF (k), bandQ (k), off); }
                if (cur[k] != 0.0f)
                    for (int i = i0; i < i0 + len; ++i) { l[i] = dynF[k].tick (l[i], 0); r[i] = dynF[k].tick (r[i], 1); }
            }
            dynDb[(size_t) k] = cur[k];
        }
    }
};

class StudioDyn : public BuiltinEffect
{
public:
    StudioDyn() { initDefaults(); }
    const char* typeId() const override { return "stdyn"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "threshold", "Threshold", -60, 0, -20 }, { "ratio", "Ratio", 1, 100, 3 }, { "knee", "Knee", 0, 30, 6 },
            { "attack", "Attack ms", 0.01f, 300, 10 }, { "release", "Release ms", 5, 4000, 80 }, { "gain", "Gain", -12, 40, 0 },
            { "detect", "Detect", 0, 1, 0, { "Peak", "RMS" } }, { "scHpf", "SC HPF", 20, 500, 20 }, { "mix", "Mix", 0, 1, 1 } };
        return ps;
    }
    void prepare (double s, int) override { core.prepare (s); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    { core.run (b, n, p (0), p (1), p (2), p (3), p (4), p (6) > 0.5f, p (7), p (5), p (8)); }
    float gainReduction() const { return core.gr; }
    float meter (int) const override { return core.gr; }
private:
    CompCore core;
};

class Fet76 : public BuiltinEffect
{
public:
    Fet76() { initDefaults(); }
    const char* typeId() const override { return "fet76"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "input", "Input", 0, 48, 24 }, { "output", "Output", -24, 24, 0 },
            { "attack", "Attack", 1, 7, 4 }, { "release", "Release", 1, 7, 4 },
            { "ratio", "Ratio", 0, 4, 0, { "4:1", "8:1", "12:1", "20:1", "All" } }, { "mix", "Mix", 0, 1, 1 } };
        return ps;
    }
    void prepare (double s, int) override { core.prepare (s); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        static const float ratios[] { 4, 8, 12, 20, 30 };
        const int ri = (int) p (4);
        const float in = dB (p (0) - 24.0f);
        b.applyGain (0, n, in);
        // 1 = slow, 7 = fastest, like the hardware
        const float att = juce::jmap (p (2), 1.0f, 7.0f, 0.8f, 0.02f), rel = juce::jmap (p (3), 1.0f, 7.0f, 1100.0f, 50.0f);
        core.run (b, n, -18.0f, ratios[ri], ri == 4 ? 0.0f : 2.0f, att, rel, false, 20, 0, p (5));
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        const float drive = ri == 4 ? 1.6f : 1.15f, og = dB (p (1));
        for (int i = 0; i < n; ++i) { l[i] = std::tanh (l[i] * drive) / drive * og; r[i] = std::tanh (r[i] * drive) / drive * og; }
    }
private:
    CompCore core;
};

class StudioGate : public BuiltinEffect
{
public:
    StudioGate() { initDefaults(); }
    const char* typeId() const override { return "stgate"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "mode", "Mode", 0, 1, 0, { "Gate", "Expander" } }, { "threshold", "Threshold", -80, 0, -40 },
            { "ratio", "Exp Ratio", 1, 10, 2 }, { "range", "Range", -80, 0, -40 }, { "attack", "Attack ms", 0.01f, 50, 0.5f },
            { "hold", "Hold ms", 0, 500, 20 }, { "release", "Release ms", 5, 2000, 120 }, { "scHpf", "SC HPF", 20, 2000, 20 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; g = 1; holdLeft = 0; sc.clear(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        sc.set (Biquad::HighPass, sr, p (7), 0.707, 0);
        const float th = p (1), floorG = dB (p (3));
        const float att = std::exp (-1.0f / (0.001f * p (4) * (float) sr)), rel = std::exp (-1.0f / (0.001f * p (6) * (float) sr));
        const int hold = (int) (p (5) * 0.001f * (float) sr);
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            float a = std::max (std::abs (l[i]), std::abs (r[i]));
            if (p (7) > 21) a = std::max (std::abs (sc.tick (l[i], 0)), std::abs (sc.tick (r[i], 1)));
            const float lvl = toDb (a);
            float t = 1.0f;
            if (lvl > th) holdLeft = hold;
            else if (holdLeft > 0) --holdLeft;
            else t = p (0) < 0.5f ? floorG : std::max (floorG, dB ((lvl - th) * (p (2) - 1.0f)));
            g = t > g ? att * g + (1 - att) * t : rel * g + (1 - rel) * t;
            l[i] *= g; r[i] *= g;
        }
    }
private:
    double sr = 44100; float g = 1; int holdLeft = 0; Biquad sc;
};

class StudioLimiter : public BuiltinEffect
{
public:
    StudioLimiter() { initDefaults(); }
    const char* typeId() const override { return "stlimit"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "gain", "Gain", 0, 24, 0 }, { "ceiling", "Ceiling", -6, 0, -1 },
                                                 { "release", "Release ms", 1, 1000, 50 }, { "lookahead", "Look-ahead", 0, 1, 0, { "Off (0 latency)", "1.5 ms" } } };
        return ps;
    }
    int latencySamples() const override { return p (3) > 0.5f ? la : 0; }
    void prepare (double s, int) override { sr = s; la = (int) (s * 0.0015); dl.setSize (2, la + 1); dl.clear(); w = 0; g = 1; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float in = dB (p (0)), ceil = dB (p (1)), rel = std::exp (-1.0f / (0.001f * p (2) * (float) sr));
        const bool look = p (3) > 0.5f;
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        auto* bl = dl.getWritePointer (0); auto* br = dl.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float xl = l[i] * in, xr = r[i] * in;
            // inter-sample-safe estimate: peak of sample and midpoint with previous
            const float a = std::max ({ std::abs (xl), std::abs (xr), std::abs (0.5f * (xl + pl)) * 1.05f, std::abs (0.5f * (xr + pr)) * 1.05f });
            pl = xl; pr = xr;
            const float t = a > ceil ? ceil / a : 1.0f;
            g = t < g ? t : rel * g + (1 - rel) * t;
            float ol = xl, orr = xr;
            if (look) { ol = bl[w]; orr = br[w]; bl[w] = xl; br[w] = xr; if (++w > la) w = 0; }
            l[i] = juce::jlimit (-ceil, ceil, ol * g); r[i] = juce::jlimit (-ceil, ceil, orr * g);
        }
    }
private:
    double sr = 44100; int la = 64, w = 0; juce::AudioBuffer<float> dl; float g = 1, pl = 0, pr = 0;
};

class Saturator : public BuiltinEffect
{
public:
    Saturator() { initDefaults(); }
    const char* typeId() const override { return "satur"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "drive", "Drive", 0, 24, 6 }, { "char", "Character", 0, 2, 0, { "Tape", "Tube", "Transformer" } },
                                                 { "mix", "Mix", 0, 1, 1 }, { "out", "Output", -24, 6, 0 } };
        return ps;
    }
    void prepare (double s, int mb) override
    {
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false);
        os->initProcessing ((size_t) mb); juce::ignoreUnused (s);
        dry.setSize (2, mb);
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        if (! os || n > dry.getNumSamples()) return;
        for (int c = 0; c < 2; ++c) dry.copyFrom (c, 0, b, c, 0, n);
        juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), 2, (size_t) n);
        auto up = os->processSamplesUp (blk);
        const float k = dB (p (0)); const int ch = (int) p (1);
        for (size_t c = 0; c < up.getNumChannels(); ++c)
        {
            auto* d = up.getChannelPointer (c);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                const float x = d[i] * k;
                d[i] = ch == 0 ? std::tanh (x) : ch == 1 ? (x >= 0 ? std::tanh (x) : std::tanh (0.7f * x) / 0.7f * 0.9f) : x / (1.0f + std::abs (x));
                d[i] /= std::max (1.0f, std::sqrt (k));
            }
        }
        os->processSamplesDown (blk);
        const float mix = p (2), og = dB (p (3));
        for (int c = 0; c < 2; ++c)
        {
            auto* d = b.getWritePointer (c); auto* s = dry.getReadPointer (c);
            for (int i = 0; i < n; ++i) d[i] = (s[i] * (1 - mix) + d[i] * mix) * og;
        }
    }
    int latencySamples() const override { return os ? (int) os->getLatencyInSamples() : 0; }
private:
    std::unique_ptr<juce::dsp::Oversampling<float>> os; juce::AudioBuffer<float> dry;
};

class ChannelStrip : public BuiltinEffect
{
public:
    ChannelStrip() { initDefaults(); }
    const char* typeId() const override { return "strip"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "gateTh", "Gate", -80, 0, -80 }, { "compTh", "Comp Thresh", -40, 0, -18 }, { "compRatio", "Comp Ratio", 1, 10, 3 },
            { "compAtt", "Comp Attack", 0.1f, 50, 5 }, { "compRel", "Comp Release", 20, 1000, 100 }, { "makeup", "Makeup", 0, 24, 3 },
            { "hpf", "HPF", 20, 400, 80 }, { "low", "Low", -15, 15, 0 }, { "mid", "Mid", -15, 15, 0 }, { "midF", "Mid Freq", 200, 6000, 1500 },
            { "high", "High", -15, 15, 0 }, { "out", "Output", -18, 18, 0 } };
        return ps;
    }
    void prepare (double s, int) override { sr = s; core.prepare (s); for (auto& f : eq) f.clear(); gate = 1; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        if (p (0) > -79)
        {
            const float th = dB (p (0)), rel = std::exp (-1.0f / (0.08f * (float) sr));
            for (int i = 0; i < n; ++i)
            {
                const float t = std::max (std::abs (l[i]), std::abs (r[i])) > th ? 1.0f : 0.0f;
                gate = t > gate ? t : rel * gate; l[i] *= gate; r[i] *= gate;
            }
        }
        core.run (b, n, p (1), p (2), 6, p (3), p (4), true, 60, p (5), 1);
        eq[0].set (Biquad::HighPass, sr, p (6), 0.707, 0); eq[1].set (Biquad::LowShelf, sr, 120, 0.707, p (7));
        eq[2].set (Biquad::Peak, sr, p (9), 1.0, p (8)); eq[3].set (Biquad::HighShelf, sr, 8000, 0.707, p (10));
        const float g = dB (p (11));
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = eq[0].tick (d[i], ch);
                if (p (7) != 0) x = eq[1].tick (x, ch);
                if (p (8) != 0) x = eq[2].tick (x, ch);
                if (p (10) != 0) x = eq[3].tick (x, ch);
                d[i] = x * g;
            }
        }
    }
private:
    double sr = 44100; CompCore core; Biquad eq[4]; float gate = 1;
};

std::unique_ptr<BuiltinEffect> createStudioEffect (const juce::String& id)
{
    if (id == "steq7") return std::make_unique<StudioEQ7>();
    if (id == "stdyn") return std::make_unique<StudioDyn>();
    if (id == "fet76") return std::make_unique<Fet76>();
    if (id == "stgate") return std::make_unique<StudioGate>();
    if (id == "stlimit") return std::make_unique<StudioLimiter>();
    if (id == "satur") return std::make_unique<Saturator>();
    if (id == "strip") return std::make_unique<ChannelStrip>();
    return {};
}
}
