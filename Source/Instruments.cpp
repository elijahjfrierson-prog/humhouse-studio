#include "Instruments.h"
#include "DrumSynth.h"
#include <cmath>

namespace hhs
{
static inline double keyToHz (double key) { return 440.0 * std::pow (2.0, (key - 69.0) / 12.0); }

// ------------------------------------------------------------ sampler
class SamplerInstrument : public ChannelInstrument
{
public:
    explicit SamplerInstrument (SharedSample s) : sample (std::move (s)) {}
    void prepare (double sr, int) override { sampleRate = sr; }
    void update (const Channel& c) override
    {
        rootKey = c.rootKey; keyTrack = c.keyTrack; pitch = c.stretchMode == 0 ? c.pitch : 0.0f; glide = c.glide; mono = c.mono;
        oneShot = c.oneShot; cutItself = c.cutItself; attack = c.ampAttack; hold = c.ampHold; release = c.ampRelease;
    }
    void noteOn (int key, float vel, bool slide) override
    {
        if (! sample || sample->getNumSamples() == 0) return;
        const double target = (keyTrack ? key - rootKey : 0) + pitch;   // key off: the sample stays the sample
        if (cutItself && ! (glide > 0.0f && (mono || slide))) choke();
        if (mono || slide)
        {
            for (auto& v : voices) if (v.active && ! v.releasing)
            {
                if (glide > 0.0f) { v.targetSemis = target; v.key = key; v.glideRate = 1.0 / (glide * sampleRate); return; }
                v.releasing = true; v.fadeLen = juce::jmax (v.fadeLen, 0.006f);
            }
        }
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr) for (auto& c : voices) if (c.releasing) { v = &c; break; }   // steal a fading voice before a held one
        if (v == nullptr) { v = &voices[(size_t) next]; next = (next + 1) % (int) voices.size(); }
        *v = {};
        v->active = true; v->key = key; v->gain = vel; v->fadeLen = juce::jmax (0.002f, release);
        v->semis = (glide > 0.0f && lastSemis > -900.0 && (mono || slide)) ? lastSemis : target;
        v->targetSemis = target;
        v->glideRate = glide > 0.0f ? 1.0 / (glide * sampleRate) : 1.0;
        lastSemis = target;
    }
    void noteOnAt (int key, float vel, bool slide, double secondsIn) override
    {
        noteOn (key, vel, slide);
        if (secondsIn <= 0.0) return;
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.key == key && v.pos == 0.0)
            {
                v.pos = secondsIn * sampleRate * std::pow (2.0, v.semis / 12.0);
                v.midStart = true;
                if (sample && v.pos >= sample->getNumSamples() - 2) v.active = false;
                break;
            }
    }
    void noteOff (int key) override
    {
        for (auto& v : voices) if (v.active && v.key == key && (mono || ! oneShot)) v.releasing = true;
    }
    void allNotesOff() override { for (auto& v : voices) v.active = false; }
    void choke() override   // 6 ms: an 808 sub cut any faster clicks; voices already in a long release get cut too
    {
        for (auto& v : voices) if (v.active) { v.fadeLen = v.releasing ? juce::jmin (v.fadeLen, 0.006f) : 0.006f; v.releasing = true; }
    }
    bool isActive() const override { for (auto& v : voices) if (v.active) return true; return false; }

    void render (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! sample) return;
        const int len = sample->getNumSamples();
        const float* l = sample->getReadPointer (0);
        const float* r = sample->getReadPointer (sample->getNumChannels() > 1 ? 1 : 0);
        auto* oL = out.getWritePointer (0);
        auto* oR = out.getWritePointer (1);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            for (int i = start; i < start + num; ++i)
            {
                if (v.semis != v.targetSemis)
                {
                    const double d = v.targetSemis - v.semis;
                    const double stepS = std::abs (d) * v.glideRate * 4.0 + 1.0e-5;
                    v.semis = std::abs (d) <= stepS ? v.targetSemis : v.semis + (d > 0 ? stepS : -stepS);
                }
                const int i0 = (int) v.pos;
                if (i0 + 1 >= len) { v.active = false; break; }
                const float f = (float) (v.pos - i0);
                float g = v.gain;
                const double ramp = std::max ((double) attack, v.midStart ? 0.004 : 0.0) * sampleRate;   // hits from the top play exactly like the file; mid-sample starts get 4 ms
                if (v.age < ramp) g *= (float) ((v.age + 1.0) / ramp);
                const double left = (double) (len - 1) - v.pos, tailN = 0.002 * sampleRate;   // samples that end above zero don't click
                if (left < tailN) g *= (float) juce::jmax (0.0, left / tailN);
                if (hold > 0.0f && ! v.releasing && v.age > hold * sampleRate) v.releasing = true;
                v.age += 1.0;
                if (v.releasing) { v.rel -= 1.0f / (v.fadeLen * (float) sampleRate); if (v.rel <= 0.0f) { v.active = false; break; } g *= v.rel; }
                auto herm = [&] (const float* x)   // 4-point Hermite: pitched hits stay smooth instead of gritty/blocky
                {
                    const float xm = x[i0 > 0 ? i0 - 1 : 0], x0 = x[i0], x1 = x[i0 + 1], x2 = x[i0 + 2 < len ? i0 + 2 : i0 + 1];
                    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
                    return ((c3 * f + c2) * f + c1) * f + x0;
                };
                oL[i] += herm (l) * g;
                oR[i] += herm (r) * g;
                v.pos += std::pow (2.0, v.semis / 12.0);
            }
        }
    }
private:
    struct Voice { bool active = false, releasing = false, midStart = false; int key = 0; double pos = 0, semis = 0, targetSemis = 0, glideRate = 1, age = 0; float gain = 1, rel = 1, fadeLen = 0.01f; };
    SharedSample sample;
    std::array<Voice, 32> voices {};
    int next = 0, rootKey = 60;
    bool keyTrack = true;
    double sampleRate = 44100.0, lastSemis = -999.0;
    float pitch = 0, glide = 0; bool mono = false;
    bool oneShot = true, cutItself = false; float attack = 0, hold = 0, release = 0.01f;
};

// ------------------------------------------------------------ synth
class SynthInstrument : public ChannelInstrument
{
public:
    void prepare (double sr, int) override { sampleRate = sr; }
    void update (const Channel& c) override { p = c.synth; glide = c.glide; mono = c.mono; pitch = c.pitch; }

    void noteOn (int key, float vel, bool slide) override
    {
        const double k = key + pitch;
        if ((mono || slide) && glide > 0.0f)
            for (auto& v : voices) if (v.active && v.stage < 3)
            { v.target = k; v.key = key; v.velocity = vel; v.glideRate = 1.0 / (glide * sampleRate); lastKey = k; return; }
        if (mono) for (auto& v : voices) if (v.active) v.stage = 3;
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr) for (auto& c : voices) if (c.stage >= 3) { v = &c; break; }   // steal a releasing voice before a held one
        if (v == nullptr) { v = &voices[(size_t) next]; next = (next + 1) % (int) voices.size(); }
        *v = {};
        v->active = true; v->key = key; v->velocity = vel; v->target = k;
        v->current = (glide > 0.0f && lastKey > 0 && (mono || slide)) ? lastKey : k;
        v->glideRate = glide > 0.0f ? 1.0 / (glide * sampleRate) : 1.0;
        lastKey = k;
    }
    void noteOff (int key) override { for (auto& v : voices) if (v.active && v.key == key && v.stage < 3) v.stage = 3; }
    void allNotesOff() override { for (auto& v : voices) v.active = false; }
    bool isActive() const override { for (auto& v : voices) if (v.active) return true; return false; }

    void render (juce::AudioBuffer<float>& out, int start, int num) override
    {
        auto* oL = out.getWritePointer (0);
        auto* oR = out.getWritePointer (1);
        const float sr = (float) sampleRate;
        const float aInc = 1.0f / std::max (1.0f, p.attack * sr);
        const float dCoef = std::exp (-1.0f / std::max (1.0f, p.decay * sr * 0.3f));
        const float rCoef = std::exp (-1.0f / std::max (1.0f, p.release * sr * 0.3f));
        const float g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (20.0f, sr * 0.45f, p.cutoff) / sr);
        const float kq = 2.0f - 2.0f * juce::jlimit (0.0f, 0.97f, p.resonance);
        const float a1 = 1.0f / (1.0f + g * (g + kq)), a2 = g * a1, a3 = g * a2;
        const float detRatio = std::pow (2.0f, p.detune / 1200.0f);

        for (auto& v : voices)
        {
            if (! v.active) continue;
            for (int i = start; i < start + num; ++i)
            {
                if (v.current != v.target)
                {
                    const double d = v.target - v.current, stepS = std::abs (d) * v.glideRate * 4.0 + 1.0e-5;
                    v.current = std::abs (d) <= stepS ? v.target : v.current + (d > 0 ? stepS : -stepS);
                }
                // envelope
                switch (v.stage)
                {
                    case 0: v.env += aInc; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 1; } break;
                    case 1: v.env = p.sustain + (v.env - p.sustain) * dCoef; if (p.wave == 4 && v.env < 0.001f) v.active = false; break;
                    case 2: break;
                    default: v.env *= rCoef; if (v.env < 0.0005f) v.active = false; break;
                }
                if (! v.active) break;
                double hz = keyToHz (v.current);
                if (p.wave == 4) hz *= 1.0 + 1.5 * std::exp (-v.age * 40.0 / sampleRate); // 808 pitch drop
                if (p.vibrato > 0.0f && p.lfoRate > 0.0f)
                    hz *= std::pow (2.0, p.vibrato * std::sin (juce::MathConstants<double>::twoPi * p.lfoRate * v.age / sampleRate) / 12.0);
                const float dt = (float) (hz / sampleRate);
                float s = p.wave >= 5 ? voiceOsc (v, dt) : osc (v.ph1, dt);
                if (p.wave < 5 && (p.detune != 0.0f || p.unison > 1)) s = 0.5f * (s + osc (v.ph2, dt * detRatio));
                if (p.noise > 0.0f) s += p.noise * nextNoise (v);
                v.ph1 += dt; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                v.ph2 += dt * detRatio; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                if (p.drive > 0.0f) s = std::tanh (s * (1.0f + p.drive * 8.0f)) / std::tanh (1.0f + p.drive * 8.0f) ;
                // TPT state-variable lowpass
                const float v3 = s - v.ic2, v1 = a1 * v.ic1 + a2 * v3, v2 = v.ic2 + a2 * v.ic1 + a3 * v3;
                v.ic1 = 2.0f * v1 - v.ic1; v.ic2 = 2.0f * v2 - v.ic2;
                const float y = v2 * v.env * v.velocity * 0.9f;
                oL[i] += y; oR[i] += y;
                ++v.age;
            }
        }
    }
private:
    struct Voice { bool active = false; int key = 0, stage = 0; double current = 60, target = 60, glideRate = 1, age = 0;
                   float env = 0, velocity = 1, ph1 = 0, ph2 = 0.25f, ic1 = 0, ic2 = 0, fmPh = 0;
                   std::array<float, 6> sx { 0.11f, 0.37f, 0.53f, 0.71f, 0.89f, 0.23f };
                   uint32_t rng = 0x9e3779b9u; };
    static float nextNoise (Voice& v)
    {
        v.rng = v.rng * 1664525u + 1013904223u;
        return (float) (v.rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    float voiceOsc (Voice& v, float dt)
    {
        constexpr float tp = juce::MathConstants<float>::twoPi;
        const double t = v.age / sampleRate;
        switch (p.wave)
        {
            case 5: // supersaw: 7 detuned saws
            {
                static constexpr float spread[6] { -1.0f, -0.62f, -0.3f, 0.3f, 0.62f, 1.0f };
                const float amt = 0.004f + p.detune * 0.03f;
                float s = 2.0f * v.ph1 - 1.0f - blep (v.ph1, dt);
                for (size_t k = 0; k < 6; ++k)
                {
                    const float d = dt * (1.0f + spread[k] * amt);
                    float& ph = v.sx[k];
                    s += 0.7f * (2.0f * ph - 1.0f - blep (ph, d));
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                }
                return s * 0.3f;
            }
            case 6: // 2-op FM, index decays like a struck tine / bell
            {
                const float idx = p.fmIndex * (0.25f + 0.75f * (float) std::exp (-t * 2.5));
                const float m = std::sin (tp * v.fmPh);
                v.fmPh += dt * p.fmRatio; v.fmPh -= std::floor (v.fmPh);
                return std::sin (tp * v.ph1 + idx * m);
            }
            case 7: // morphing wavetable swept by the LFO
            {
                const float m = juce::jlimit (0.0f, 1.0f, p.morph + 0.35f * std::sin (tp * (float) (p.lfoRate * t))) * 3.0f;
                const float w[4] { std::sin (tp * v.ph1), 4.0f * std::abs (v.ph1 - 0.5f) - 1.0f,
                                   2.0f * v.ph1 - 1.0f - blep (v.ph1, dt), v.ph1 < 0.5f ? 1.0f : -1.0f };
                const int i0 = juce::jmin (2, (int) m);
                return w[i0] + (w[i0 + 1] - w[i0]) * (m - (float) i0);
            }
            default: // air: noise over a sine body
                return 0.5f * std::sin (tp * v.ph1) + 0.5f * nextNoise (v);
        }
    }
    static inline float blep (float t, float dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    inline float osc (float ph, float dt) const
    {
        switch (p.wave)
        {
            case 0: return 2.0f * ph - 1.0f - blep (ph, dt);
            case 1: { float s = ph < 0.5f ? 1.0f : -1.0f; s += blep (ph, dt); float t2 = ph + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f; return s - blep (t2, dt); }
            case 2: return 4.0f * std::abs (ph - 0.5f) - 1.0f;
            default: return std::sin (juce::MathConstants<float>::twoPi * ph);
        }
    }
    SynthParams p;
    std::array<Voice, 24> voices {};
    int next = 0; double sampleRate = 44100.0, lastKey = -1; float glide = 0, pitch = 0; bool mono = false;
};

std::unique_ptr<ChannelInstrument> createInstrument (const Channel& c, SharedSample sample, double sr)
{
    std::unique_ptr<ChannelInstrument> inst;
    if (c.type == ChannelType::Synth) inst = std::make_unique<SynthInstrument>();
    else if (c.type == ChannelType::Sampler)
    {
        if (! sample && c.builtinDrum >= 0)
            sample = std::make_shared<const juce::AudioBuffer<float>> (renderBuiltinDrum (c.builtinDrum, sr));
        inst = std::make_unique<SamplerInstrument> (sample);
    }
    if (inst) { inst->prepare (sr, 4096); inst->update (c); }
    return inst;
}

// Band-limited windowed-sinc sample-rate conversion (64 taps, Blackman). Low-passes below the lower
// Nyquist so a 96k/48k file played at 44.1k doesn't fold its highs back down as metallic "notes".
void sincResample (const float* in, int inLen, float* out, int outLen, double ratio)
{
    constexpr int half = 32, res = 512;
    const double fc = juce::jmin (1.0, 1.0 / ratio) * 0.97;
    static thread_local std::vector<float> tab;
    static thread_local double tabFc = -1.0;
    if (tabFc != fc)
    {
        tab.assign ((size_t) (half * res + 2), 0.0f);
        for (int i = 0; i <= half * res; ++i)
        {
            const double x = (double) i / res, px = juce::MathConstants<double>::pi * x;
            const double sinc = i == 0 ? fc : std::sin (px * fc) / px;
            const double w = 0.42 + 0.5 * std::cos (px / half) + 0.08 * std::cos (2.0 * px / half);
            tab[(size_t) i] = (float) (sinc * w);
        }
        tabFc = fc;
    }
    for (int o = 0; o < outLen; ++o)
    {
        const double t = o * ratio;
        const int c = (int) std::floor (t);
        double acc = 0.0, ws = 0.0;
        for (int k = c - half + 1; k <= c + half; ++k)
        {
            const double p = std::abs (t - k) * res;
            const int ip = (int) p;
            if (ip >= half * res) continue;
            const double w = tab[(size_t) ip] + (p - ip) * (tab[(size_t) ip + 1] - tab[(size_t) ip]);
            ws += w;
            if (k >= 0 && k < inLen) acc += w * in[k];
        }
        out[o] = ws != 0.0 ? (float) (acc / ws) : 0.0f;
    }
}

SharedSample loadSampleFile (const juce::File& f, double targetRate)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r || r->lengthInSamples <= 0) return {};
    const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) r->sampleRate * 60);
    juce::AudioBuffer<float> raw (2, len);
    r->read (&raw, 0, len, 0, true, true);
    if (std::abs (r->sampleRate - targetRate) < 1.0) return std::make_shared<const juce::AudioBuffer<float>> (std::move (raw));
    const double ratio = r->sampleRate / targetRate;
    const int outLen = (int) (len / ratio);
    juce::AudioBuffer<float> res (2, outLen);
    for (int ch = 0; ch < 2; ++ch)
        sincResample (raw.getReadPointer (ch), len, res.getWritePointer (ch), outLen, ratio);
    return std::make_shared<const juce::AudioBuffer<float>> (std::move (res));
}

static juce::AudioBuffer<float> olaStretch (const juce::AudioBuffer<float>& in, double factor)
{
    const int N = 2048, hopOut = N / 4, inLen = in.getNumSamples(), nch = in.getNumChannels();
    const double hopIn = hopOut / factor;
    const int outLen = juce::jmax (1, (int) (inLen * factor));
    juce::AudioBuffer<float> out (nch, outLen + N); out.clear();
    std::vector<float> win ((size_t) N), norm ((size_t) (outLen + N), 0.0f);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);
    // WSOLA: each grain is taken where its waveform lines up with the one before it (within one hop), so
    // stretched 808s, keys and vocals stay in phase instead of going hollow/flammy; grain 0 starts exactly
    // at the top of the file, so the hit lands on time like FL
    std::vector<float> mono ((size_t) inLen);
    for (int i = 0; i < inLen; ++i) { float m = 0; for (int ch = 0; ch < nch; ++ch) m += in.getSample (ch, i); mono[(size_t) i] = m; }
    const int tol = hopOut, cmpLen = N / 2;
    int prevIp = 0;
    for (int frame = 0;; ++frame)
    {
        const int o = frame * hopOut;
        if (o >= outLen) break;
        int ip = (int) (frame * hopIn);
        if (frame > 0 && inLen > N)
        {
            const int natural = prevIp + hopOut;
            double best = -1.0e30; int bestIp = ip;
            for (int c = std::max (0, ip - tol); c <= std::min (inLen - N, ip + tol); c += 2)
            {
                double acc = 0;
                for (int j = 0; j < cmpLen && natural + j < inLen; j += 2) acc += mono[(size_t) (natural + j)] * mono[(size_t) (c + j)];
                if (acc > best) { best = acc; bestIp = c; }
            }
            ip = bestIp;
        }
        prevIp = ip;
        for (int i = 0; i < N && ip + i < inLen; ++i)
        {
            for (int ch = 0; ch < nch; ++ch) out.addSample (ch, o + i, in.getSample (ch, ip + i) * win[(size_t) i]);
            norm[(size_t) (o + i)] += win[(size_t) i];
        }
    }
    for (int i = 0; i < outLen; ++i)
        if (norm[(size_t) i] > 1.0e-3f) for (int ch = 0; ch < nch; ++ch) out.setSample (ch, i, out.getSample (ch, i) / norm[(size_t) i]);
    out.setSize (nch, outLen, true);
    return out;
}

SharedSample processSample (SharedSample src, const Channel& c, double sr)
{
    if (! src || src->getNumSamples() == 0) return src;
    const bool stretch = c.stretchMode != 0 && (std::abs (c.stretchTime - 1.0f) > 1.0e-3f || std::abs (c.pitch) > 1.0e-3f);
    if (! (c.smpNormalize || c.smpReverse || c.smpInvert || c.smpDeclick || c.smpTrim || c.smpBoostDb != 0.0f
           || c.smpFadeIn > 0.0f || c.smpFadeOut > 0.0f || stretch))
        return src;
    juce::AudioBuffer<float> b (*src);
    const int nch = b.getNumChannels();
    if (c.smpTrim)
    {
        const float thr = juce::Decibels::decibelsToGain (-60.0f);
        auto loud = [&] (int i) { for (int ch = 0; ch < nch; ++ch) if (std::abs (b.getSample (ch, i)) > thr) return true; return false; };
        int s0 = 0, s1 = b.getNumSamples() - 1;
        while (s0 < s1 && ! loud (s0)) ++s0;
        while (s1 > s0 && ! loud (s1)) --s1;
        juce::AudioBuffer<float> t (nch, s1 - s0 + 1);
        for (int ch = 0; ch < nch; ++ch) t.copyFrom (ch, 0, b, ch, s0, t.getNumSamples());
        b = std::move (t);
    }
    if (c.smpReverse) b.reverse (0, b.getNumSamples());
    float gain = juce::Decibels::decibelsToGain (c.smpBoostDb) * (c.smpInvert ? -1.0f : 1.0f);
    if (c.smpNormalize)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < nch; ++ch) peak = juce::jmax (peak, b.getMagnitude (ch, 0, b.getNumSamples()));
        if (peak > 1.0e-6f) gain *= 0.98f / peak;
    }
    b.applyGain (gain);
    if (stretch)
    {
        const double ratio = std::pow (2.0, c.pitch / 12.0);
        auto st = olaStretch (b, juce::jlimit (0.05, 20.0, (double) c.stretchTime * ratio));
        const int outLen = juce::jmax (1, (int) (st.getNumSamples() / ratio));
        juce::AudioBuffer<float> res (nch, outLen);
        for (int ch = 0; ch < nch; ++ch) { juce::LagrangeInterpolator li; li.process (ratio, st.getReadPointer (ch), res.getWritePointer (ch), outLen); }
        b = std::move (res);
    }
    const int n = b.getNumSamples();
    const int fi = juce::jmax (c.smpDeclick ? (int) (0.002 * sr) : 0, (int) (c.smpFadeIn * sr));
    const int fo = juce::jmax (c.smpDeclick ? (int) (0.002 * sr) : 0, (int) (c.smpFadeOut * sr));
    if (fi > 0) b.applyGainRamp (0, juce::jmin (fi, n), 0.0f, 1.0f);
    if (fo > 0) { const int len = juce::jmin (fo, n); b.applyGainRamp (n - len, len, 1.0f, 0.0f); }
    return std::make_shared<const juce::AudioBuffer<float>> (std::move (b));
}
}
