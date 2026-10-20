// Studio-grade console set: own DSP modelled on classic stock-plugin workflows.
#include "Effects.h"
#include <cmath>
#include <complex>
#include <string>

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

// Parametric EQ (FL Parametric EQ 2 style): 12 bands, 7 on by default. Natural = zero-latency IIR,
// Linear = linear-phase FIR of the same curve (adds firN/2 latency). Bell / shelf bands can be dynamic (zero latency).
class StudioEQ7 : public BuiltinEffect
{
public:
    static constexpr int firOrder = 11, firN = 1 << firOrder, numBands = 12, perBand = 10, bandBase = 2, phaseIdx = 1;
    enum BandType { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPassT };
    StudioEQ7() { initDefaults(); }
    const char* typeId() const override { return "steq7"; }
    const std::vector<ParamDef>& params() const override
    {
        static std::vector<std::unique_ptr<std::string>> store;
        static const std::vector<ParamDef> ps = [] {
            auto str = [] (const std::string& t) { store.push_back (std::make_unique<std::string> (t)); return store.back()->c_str(); };
            std::vector<ParamDef> v { { "out", "Output", -18, 18, 0 }, { "phase", "Phase", 0, 1, 0, { "Natural (0 latency)", "Linear (~23 ms)" } } };
            const int   types[] { LowCut, LowShelf, Bell, Bell, Bell, Bell, HighShelf, Bell, Bell, Bell, Bell, Bell };
            const float freqs[] { 30, 100, 250, 1000, 3500, 8000, 12000, 500, 2000, 5000, 700, 1500 };
            for (int k = 0; k < numBands; ++k)
            {
                const std::string e = "e" + std::to_string (k + 1), n = "Band " + std::to_string (k + 1);
                const float q = types[k] == Bell ? 1.0f : 0.707f;
                v.push_back ({ str (e + "On"), str (n + " On"), 0, 1, k < 7 ? 1.0f : 0.0f, { "Off", "On" } });
                v.push_back ({ str (e + "T"), str (n + " Type"), 0, 6, (float) types[k], { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" } });
                v.push_back ({ str (e + "F"), str (n + " Freq"), 20, 20000, freqs[k] });
                v.push_back ({ str (e + "G"), str (n + " Gain"), -18, 18, 0 });
                v.push_back ({ str (e + "Q"), str (n + " Q"), 0.1f, 18, q });
                v.push_back ({ str (e + "Dyn"), str (n + " Dynamic"), 0, 1, 0, { "Off", "On" } });
                v.push_back ({ str (e + "DTh"), str (n + " Dyn Thresh"), -60, 0, -24 });
                v.push_back ({ str (e + "DRg"), str (n + " Dyn Range"), -18, 18, -6 });
                v.push_back ({ str (e + "DAt"), str (n + " Dyn Attack"), 0.1f, 100, 5 });
                v.push_back ({ str (e + "DRl"), str (n + " Dyn Release"), 5, 1000, 120 });
            }
            v.push_back ({ "solo", "Listen band", 0, 12, 0 });
            return v; }();
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
        const int so = (int) std::round (p (soloIdx));
        if (so > 0 && so <= numBands)   // Pro-Q style listen: hear only the region the band works on
        {
            soloF.set (Biquad::BandPass, sr, juce::jlimit (20.0f, 20000.0f, bp (so - 1, 2)), juce::jmax (0.3f, bp (so - 1, 4)), 0);
            for (int c = 0; c < juce::jmin (2, b.getNumChannels()); ++c)
            {
                auto* x = b.getWritePointer (c);
                for (int i = 0; i < n; ++i) x[i] = soloF.tick (x[i], c);
            }
        }
        else if (lastSolo > 0) soloF.clear();
        lastSolo = so;
    }
private:
    double sr = 44100; int maxN = 512;
    static constexpr int soloIdx = bandBase + numBands * perBand;
    Biquad f_[numBands], dynF[numBands], det[numBands], soloF;
    int lastSolo = 0;
    bool active[numBands] {};
    float hash = -1, env[numBands] {}, cur[numBands] {};
    std::array<std::atomic<float>, numBands> dynDb {};
    std::unique_ptr<juce::dsp::Convolution> conv;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fftData;
    bool firDirty = true; int firHold = 0;

    float bp (int k, int field) const { return p (bandBase + k * perBand + field); }
    int  typeOf (int k) const { return juce::jlimit (0, 6, (int) std::round (bp (k, 1))); }
    static bool gainType (int t) { return t == Bell || t == LowShelf || t == HighShelf; }
    void setBand (Biquad& q, int t, float f, float qq, float g) const
    {
        switch (t)
        {
            case LowShelf:  q.set (Biquad::LowShelf, sr, f, qq, g); break;
            case HighShelf: q.set (Biquad::HighShelf, sr, f, qq, g); break;
            case LowCut:    q.set (Biquad::HighPass, sr, f, qq, 0); break;
            case HighCut:   q.set (Biquad::LowPass, sr, f, qq, 0); break;
            case Notch:     q.set (Biquad::Peak, sr, f, qq, -40); break;
            case BandPassT: q.set (Biquad::BandPass, sr, f, qq, 0); break;
            default:        q.set (Biquad::Peak, sr, f, qq, g); break;
        }
    }
    void update()
    {
        float h = p (0) * 1.37f;
        for (int k = 0; k < numBands; ++k) for (int fld = 0; fld < 5; ++fld) h += bp (k, fld) * (float) (k * 7 + fld + 3) * 0.917f;
        if (h == hash) return;
        hash = h;
        for (int k = 0; k < numBands; ++k)
        {
            const int t = typeOf (k);
            const float f = bp (k, 2), q = bp (k, 4), g = bp (k, 3);
            active[k] = bp (k, 0) > 0.5f && ! (gainType (t) && g == 0.0f);
            setBand (f_[k], t, f, q, g);
            det[k].set (t == LowShelf ? Biquad::LowPass : t == HighShelf ? Biquad::HighPass : Biquad::BandPass, sr, f, juce::jmax (0.5f, q), 0);
            if (gainType (t)) setBand (dynF[k], t, f, q, cur[k]);
        }
        firDirty = true;
    }
    double mag (double hz) const
    {
        using C = std::complex<double>;
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.5, hz) / sr;
        const C z1 = std::polar (1.0, -w), z2 = z1 * z1;
        C H (1.0);
        for (int k = 0; k < numBands; ++k)
            if (active[k])
            {
                const auto& q = f_[k];
                H *= (C (q.b0) + C (q.b1) * z1 + C (q.b2) * z2) / (C (1.0) + C (q.a1) * z1 + C (q.a2) * z2);
            }
        return std::abs (H) * dB (p (0));
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
        const float g = dB (p (0));
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                for (int k = 0; k < numBands; ++k) if (active[k]) x = f_[k].tick (x, ch);
                d[i] = x * g;
            }
        }
    }
    void processDynamic (juce::AudioBuffer<float>& b, int n)
    {
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int k = 0; k < numBands; ++k)
        {
            const int t = typeOf (k);
            if (bp (k, 0) < 0.5f || bp (k, 5) < 0.5f || ! gainType (t)) { cur[k] = 0; env[k] = -120.0f; dynDb[(size_t) k] = 0.0f; continue; }
            const float th = bp (k, 6), range = bp (k, 7), attMs = bp (k, 8), relMs = bp (k, 9);
            for (int i0 = 0; i0 < n; i0 += 32)
            {
                const int len = juce::jmin (32, n - i0);
                float pk = 0;
                for (int i = i0; i < i0 + len; ++i) pk = std::max (pk, std::abs (det[k].tick (0.5f * (l[i] + r[i]), 0)));
                const float lvl = toDb (pk);
                const float ca = std::exp (-(float) len / (0.001f * attMs * (float) sr)), cr = std::exp (-(float) len / (0.001f * relMs * (float) sr));
                env[k] = lvl > env[k] ? ca * env[k] + (1 - ca) * lvl : cr * env[k] + (1 - cr) * lvl;
                const float off = range * juce::jlimit (0.0f, 1.0f, (env[k] - th) / 12.0f);
                if (std::abs (off - cur[k]) > 0.05f) { cur[k] = off; setBand (dynF[k], t, bp (k, 2), bp (k, 4), off); }
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

// 3-band mastering maximizer: Linkwitz-Riley split, per-band pre/threshold/ratio/attack/release/post,
// saturation, then a zero-latency master limiter
class Maximizer : public BuiltinEffect
{
public:
    Maximizer() { initDefaults(); }
    const char* typeId() const override { return "stmax"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "xlo", "Low / Mid Hz", 40, 1000, 180 }, { "xhi", "Mid / High Hz", 1000, 12000, 4500 },
            { "lpre", "Low Pre dB", -24, 24, 0 }, { "lthr", "Low Threshold", -40, 0, -12 }, { "lratio", "Low Ratio", 1, 20, 2 },
            { "latk", "Low Attack ms", 0.1f, 100, 15 }, { "lrel", "Low Release ms", 5, 1000, 150 }, { "lpost", "Low Post dB", -24, 24, 0 },
            { "mpre", "Mid Pre dB", -24, 24, 0 }, { "mthr", "Mid Threshold", -40, 0, -12 }, { "mratio", "Mid Ratio", 1, 20, 2 },
            { "matk", "Mid Attack ms", 0.1f, 100, 10 }, { "mrel", "Mid Release ms", 5, 1000, 120 }, { "mpost", "Mid Post dB", -24, 24, 0 },
            { "hpre", "High Pre dB", -24, 24, 0 }, { "hthr", "High Threshold", -40, 0, -12 }, { "hratio", "High Ratio", 1, 20, 2 },
            { "hatk", "High Attack ms", 0.1f, 100, 5 }, { "hrel", "High Release ms", 5, 1000, 80 }, { "hpost", "High Post dB", -24, 24, 0 },
            { "sat", "Saturation", 0, 1, 0 }, { "gain", "Master Gain", 0, 24, 0 }, { "ceiling", "Ceiling", -6, 0, -0.3f },
            { "release", "Master Release ms", 1, 1000, 60 } };
        return ps;
    }
    void prepare (double s, int mb) override
    {
        sr = s;
        juce::dsp::ProcessSpec spec { s, (juce::uint32) juce::jmax (1, mb), 2 };
        for (auto* f : { &lp1, &hp1, &lp2, &hp2, &ap }) { f->prepare (spec); f->reset(); }
        lp1.setType (juce::dsp::LinkwitzRileyFilterType::lowpass); hp1.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
        lp2.setType (juce::dsp::LinkwitzRileyFilterType::lowpass); hp2.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
        ap.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        curLo = curHi = -1; g = 1; pl = pr = 0;
        for (auto& e : env) e = 0;
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float xlo = juce::jlimit (20.0f, (float) sr * 0.2f, p (0)), xhi = juce::jlimit (xlo * 1.5f, (float) sr * 0.45f, p (1));
        if (xlo != curLo) { lp1.setCutoffFrequency (xlo); hp1.setCutoffFrequency (xlo); curLo = xlo; }
        if (xhi != curHi) { lp2.setCutoffFrequency (xhi); hp2.setCutoffFrequency (xhi); ap.setCutoffFrequency (xhi); curHi = xhi; }
        float pre[3], thr[3], slope[3], atk[3], rel[3], post[3];
        for (int k = 0; k < 3; ++k)
        {
            const int o = 2 + 6 * k;
            pre[k] = dB (p (o)); thr[k] = p (o + 1); slope[k] = 1.0f - 1.0f / juce::jmax (1.0f, p (o + 2));
            atk[k] = std::exp (-1.0f / (0.001f * juce::jmax (0.1f, p (o + 3)) * (float) sr));
            rel[k] = std::exp (-1.0f / (0.001f * juce::jmax (1.0f, p (o + 4)) * (float) sr));
            post[k] = dB (p (o + 5));
        }
        const float sat = p (20), in = dB (p (21)), ceil = dB (p (22)), mrel = std::exp (-1.0f / (0.001f * juce::jmax (1.0f, p (23)) * (float) sr));
        auto* l = b.getWritePointer (0); auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            float band[3][2];
            const float x[2] { l[i], r[i] };
            for (int c = 0; c < 2; ++c)
            {
                const float rest = hp1.processSample (c, x[c]);
                band[0][c] = ap.processSample (c, lp1.processSample (c, x[c]));   // keeps the low band in phase with the mid/high split
                band[1][c] = lp2.processSample (c, rest);
                band[2][c] = hp2.processSample (c, rest);
            }
            float y[2] { 0, 0 };
            for (int k = 0; k < 3; ++k)
            {
                const float lvl = std::max (std::abs (band[k][0]), std::abs (band[k][1])) * pre[k];
                env[k] = lvl > env[k] ? atk[k] * env[k] + (1 - atk[k]) * lvl : rel[k] * env[k] + (1 - rel[k]) * lvl;
                const float over = juce::Decibels::gainToDecibels (env[k], -120.0f) - thr[k];
                const float gr = over <= -3.0f ? 0.0f : over < 3.0f ? slope[k] * (over + 3.0f) * (over + 3.0f) / 12.0f : slope[k] * over;   // 6 dB soft knee
                const float gk = pre[k] * post[k] * dB (-gr);
                y[0] += band[k][0] * gk; y[1] += band[k][1] * gk;
            }
            for (auto& v : y)
            {
                v *= in;
                if (sat > 0) v = (1.0f - sat) * v + sat * std::tanh (v);
            }
            const float a = std::max ({ std::abs (y[0]), std::abs (y[1]), std::abs (0.5f * (y[0] + pl)) * 1.05f, std::abs (0.5f * (y[1] + pr)) * 1.05f });
            pl = y[0]; pr = y[1];
            const float t = a > ceil ? ceil / a : 1.0f;
            g = t < g ? t : mrel * g + (1 - mrel) * t;
            l[i] = juce::jlimit (-ceil, ceil, y[0] * g); r[i] = juce::jlimit (-ceil, ceil, y[1] * g);
        }
    }
private:
    double sr = 44100;
    juce::dsp::LinkwitzRileyFilter<float> lp1, hp1, lp2, hp2, ap;
    float curLo = -1, curHi = -1, env[3] {}, g = 1, pl = 0, pr = 0;
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
    // zero latency: anti-derivative anti-aliasing instead of oversampling (no lookahead, no delay)
    void prepare (double s, int mb) override
    {
        juce::ignoreUnused (s);
        dry.setSize (2, mb);
        xPrev[0] = xPrev[1] = 0;
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        if (n > dry.getNumSamples()) return;
        for (int c = 0; c < 2; ++c) dry.copyFrom (c, 0, b, c, 0, n);
        const float k = dB (p (0)); const int ch = (int) p (1);
        const float norm = 1.0f / std::max (1.0f, std::sqrt (k));
        const float mix = p (2), og = dB (p (3));
        for (int c = 0; c < 2; ++c)
        {
            auto* d = b.getWritePointer (c); auto* s = dry.getReadPointer (c);
            double x1 = xPrev[c];
            for (int i = 0; i < n; ++i)
            {
                const double x = (double) s[i] * k;
                const double dx = x - x1;
                const double y = std::abs (dx) > 1.0e-5 ? (prim (x, ch) - prim (x1, ch)) / dx : shape (0.5 * (x + x1), ch);
                x1 = x;
                d[i] = (s[i] * (1 - mix) + (float) y * norm * mix) * og;
            }
            xPrev[c] = std::isfinite (x1) ? x1 : 0.0;
        }
    }
private:
    static double logCosh (double x) { const double a = std::abs (x); return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531; }
    static double shape (double x, int ch)
    {
        if (ch == 0) return std::tanh (x);
        if (ch == 1) return x >= 0 ? std::tanh (x) : std::tanh (0.7 * x) / 0.7 * 0.9;
        return x / (1.0 + std::abs (x));
    }
    static double prim (double x, int ch)
    {
        if (ch == 0) return logCosh (x);
        if (ch == 1) return x >= 0 ? logCosh (x) : 0.9 / 0.49 * logCosh (0.7 * x);
        const double a = std::abs (x); return a - std::log1p (a);
    }
    double xPrev[2] {};
    juce::AudioBuffer<float> dry;
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

// Studio Vocal: zero-latency vocal smoother. Four dynamic bell cuts (box, nasal, harsh, sibilance) only dip when
// their band rings out above the rest of the voice, stereo-linked, plus tube-style warmth and "silk" air harmonics.
class StudioVocal : public BuiltinEffect
{
public:
    StudioVocal() { initDefaults(); }
    const char* typeId() const override { return "stvocal"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps { { "smooth", "Smooth", 0, 1, 0.5f }, { "deess", "De-Ess", 0, 1, 0.4f },
                                                 { "silk", "Silk (air)", 0, 1, 0.3f }, { "warmth", "Warmth", 0, 1, 0.2f },
                                                 { "out", "Output", -12, 12, 0 }, { "mix", "Mix", 0, 1, 1 } };
        return ps;
    }
    void prepare (double s, int) override
    {
        sr = s;
        static const float fq[kBands] = { 300.0f, 1000.0f, 3200.0f, 7000.0f }, qs[kBands] = { 1.4f, 1.8f, 2.0f, 1.5f };
        for (int b = 0; b < kBands; ++b) { cut[b].set (fq[b], qs[b], sr); det[b] = cut[b]; for (auto& st : stCut[b]) st = {}; stDet[b] = {}; env[b] = 0.0f; gain[b] = 1.0f; }
        air.set (5000.0f, 0.7f, sr); stAir[0] = stAir[1] = {};
        envAll = 0.0f;
        atk = (float) (1.0 - std::exp (-1.0 / (0.003 * sr))); rel = (float) (1.0 - std::exp (-1.0 / (0.08 * sr)));
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        static const float relTh[kBands] = { -6.0f, -10.0f, -14.0f, -16.0f };
        const float amt[kBands] = { p (0), p (0), p (0), p (1) };
        const float silk = p (2), warm = p (3), og = dB (p (4)), mix = p (5);
        const float drive = 1.0f + 3.0f * warm;
        auto* L = b.getWritePointer (0); auto* R = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            envAll += (m * m - envAll) * (m * m > envAll ? atk : rel);
            float x[2] = { L[i], R[i] };
            const float dryL = x[0], dryR = x[1];
            for (int k = 0; k < kBands; ++k)
            {
                const float bp = det[k].band (stDet[k], m);
                const float e2 = bp * bp;
                env[k] += (e2 - env[k]) * (e2 > env[k] ? atk : rel);
                float target = 1.0f;
                if (amt[k] > 0.0f && envAll > 1.0e-9f)
                {
                    const float over = 10.0f * std::log10 ((env[k] + 1.0e-12f) / envAll) - relTh[k];
                    if (over > 0.0f) target = dB (-std::min (over * (0.4f + 0.6f * amt[k]), 12.0f * amt[k]));
                }
                gain[k] += (target - gain[k]) * (target < gain[k] ? atk : rel);
                for (int c = 0; c < 2; ++c) x[c] -= (1.0f - gain[k]) * cut[k].band (stCut[k][c], x[c]);
            }
            for (int c = 0; c < 2; ++c)
            {
                float y = warm > 0.0f ? std::tanh (drive * x[c]) / drive : x[c];
                if (silk > 0.0f) { const float h = air.high (stAir[c], y); y += silk * 0.5f * std::tanh (2.5f * h); }
                x[c] = y;
            }
            L[i] = (dryL * (1.0f - mix) + x[0] * mix) * og;
            R[i] = (dryR * (1.0f - mix) + x[1] * mix) * og;
        }
    }
    int latencySamples() const override { return 0; }
private:
    static constexpr int kBands = 4;
    struct St { float ic1 = 0, ic2 = 0; };
    struct Svf   // TPT state-variable filter; band() has unity peak gain
    {
        float a1 = 0, a2 = 0, a3 = 0, k = 1;
        void set (float f, float q, double s) { const float g = std::tan (juce::MathConstants<float>::pi * juce::jmin (f, (float) s * 0.45f) / (float) s); k = 1.0f / q; a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2; }
        void tick (St& s, float x, float& v1, float& v2) const { const float v3 = x - s.ic2; v1 = a1 * s.ic1 + a2 * v3; v2 = s.ic2 + a2 * s.ic1 + a3 * v3; s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2; }
        float band (St& s, float x) const { float v1, v2; tick (s, x, v1, v2); return k * v1; }
        float high (St& s, float x) const { float v1, v2; tick (s, x, v1, v2); return x - k * v1 - v2; }
    };
    Svf cut[kBands], det[kBands], air;
    St stCut[kBands][2], stDet[kBands], stAir[2];
    float env[kBands] {}, gain[kBands] { 1, 1, 1, 1 }, envAll = 0, atk = 0.01f, rel = 0.001f;
    double sr = 44100;
};

// HumHouse Vocal Room: a finished-vocal chain on six knobs - Body, Air, Compress, Room, Echo, Double.
// Zero latency; any stage with its knob at 0 is skipped, so a light setting costs almost no CPU.
class VocalRoom : public BuiltinEffect
{
public:
    VocalRoom() { initDefaults(); }
    const char* typeId() const override { return "vroom"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "body", "Body", 0, 1, 0.4f }, { "air", "Air", 0, 1, 0.4f }, { "compress", "Compress", 0, 1, 0.5f },
            { "room", "Room", 0, 1, 0.2f }, { "roomType", "Room Type", 0, 2, 1, { "Booth", "Plate", "Hall" } },
            { "echo", "Echo", 0, 1, 0.15f }, { "echoTime", "Echo Time", 0, 3, 1, { "1/16", "1/8", "1/8 dot", "1/4" } },
            { "double", "Double", 0, 1, 0 }, { "out", "Output", -12, 12, 0 } };
        return ps;
    }
    double tailSeconds() const override { return 4.0; }
    int latencySamples() const override { return 0; }
    void prepare (double s, int maxBlock) override
    {
        sr = s; core.prepare (s);
        for (auto& f : eq) f.clear();
        wet.setSize (2, juce::jmax (64, maxBlock));
        verb = createBuiltinEffect ("verb"); echo = createBuiltinEffect ("delay"); dbl = createBuiltinEffect ("chorus");
        verb->prepare (s, maxBlock); echo->prepare (s, maxBlock); dbl->prepare (s, maxBlock);
        verb->setParam ("mix", 1.0f); verb->setParam ("lowcut", 280.0f); verb->setParam ("width", 1.0f);
        echo->setParam ("mix", 1.0f); echo->setParam ("feedback", 0.32f); echo->setParam ("tone", 4500.0f); echo->setParam ("pingpong", 1.0f);
        dbl->setParam ("mix", 1.0f); dbl->setParam ("depth", 0.2f); dbl->setParam ("rate", 0.35f); dbl->setParam ("delay", 14.0f); dbl->setParam ("feedback", 0.0f);
        lastType = lastTime = -1;
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        if (n > wet.getNumSamples()) wet.setSize (2, n, false, false, true);
        const float body = p (0), air = p (1), cmp = p (2), room = p (3), ech = p (5), dub = p (7);
        eq[0].set (Biquad::HighPass, sr, 85, 0.707, 0);
        eq[1].set (Biquad::Peak, sr, 230, 0.8, 4.0 * body);
        eq[2].set (Biquad::Peak, sr, 3200, 1.0, 1.5 * air);
        eq[3].set (Biquad::HighShelf, sr, 9500, 0.707, 6.0 * air);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = eq[0].tick (d[i], ch);
                if (body > 0.0f) x = eq[1].tick (x, ch);
                if (air > 0.0f) { x = eq[2].tick (x, ch); x = eq[3].tick (x, ch); }
                d[i] = x;
            }
        }
        if (cmp > 0.0f) core.run (b, n, -12.0f - 18.0f * cmp, 2.0f + 4.0f * cmp, 8.0f, 6.0f, 110.0f, true, 120.0f, 9.0f * cmp, 1.0f);
        const int type = (int) p (4), et = (int) p (6);
        if (type != lastType)
        {
            static const float size[] { 0.3f, 0.55f, 0.82f }, damp[] { 0.6f, 0.35f, 0.45f };
            verb->setParam ("size", size[juce::jlimit (0, 2, type)]); verb->setParam ("damp", damp[juce::jlimit (0, 2, type)]); lastType = type;
        }
        if (et != lastTime) { static const float idx[] { 1, 2, 3, 4 }; echo->setParam ("time", idx[juce::jlimit (0, 3, et)]); lastTime = et; }
        // sends read the dry chain in parallel (copied first) so they don't feed each other
        if (room > 0.0f || ech > 0.0f || dub > 0.0f)
        {
            if (dry.getNumSamples() < n) dry.setSize (2, n, false, false, true);
            for (int c = 0; c < 2; ++c) dry.copyFrom (c, 0, b, c, 0, n);
            juce::AudioBuffer<float> src (dry.getArrayOfWritePointers(), 2, n);
            auto sendFrom = [&] (BuiltinEffect& fx, float amt) {
                if (amt <= 0.0f) return;
                for (int c = 0; c < 2; ++c) wet.copyFrom (c, 0, src, c, 0, n);
                juce::AudioBuffer<float> v (wet.getArrayOfWritePointers(), 2, n);
                fx.process (v, n, ctx);
                for (int c = 0; c < 2; ++c) b.addFrom (c, 0, v, c, 0, n, amt);
            };
            sendFrom (*dbl, 0.5f * dub);
            sendFrom (*verb, 0.55f * room);
            sendFrom (*echo, 0.45f * ech);
        }
        b.applyGain (0, n, dB (p (8)));
    }
private:
    double sr = 44100; CompCore core; Biquad eq[4];
    std::unique_ptr<BuiltinEffect> verb, echo, dbl;
    juce::AudioBuffer<float> wet, dry;
    int lastType = -1, lastTime = -1;
};

std::unique_ptr<BuiltinEffect> createStudioEffect (const juce::String& id)
{
    if (id == "vroom") return std::make_unique<VocalRoom>();
    if (id == "stvocal") return std::make_unique<StudioVocal>();
    if (id == "steq7") return std::make_unique<StudioEQ7>();
    if (id == "stdyn") return std::make_unique<StudioDyn>();
    if (id == "fet76") return std::make_unique<Fet76>();
    if (id == "stgate") return std::make_unique<StudioGate>();
    if (id == "stlimit") return std::make_unique<StudioLimiter>();
    if (id == "stmax") return std::make_unique<Maximizer>();
    if (id == "satur") return std::make_unique<Saturator>();
    if (id == "strip") return std::make_unique<ChannelStrip>();
    return {};
}
}
