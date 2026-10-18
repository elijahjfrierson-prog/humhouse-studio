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
        rootKey = c.rootKey; pitch = c.pitch; glide = c.glide; mono = c.mono;
    }
    void noteOn (int key, float vel, bool slide) override
    {
        if (! sample || sample->getNumSamples() == 0) return;
        const double target = key - rootKey + pitch;
        if (mono || slide)
        {
            for (auto& v : voices) if (v.active && ! v.releasing)
            {
                if (glide > 0.0f) { v.targetSemis = target; v.key = key; v.glideRate = 1.0 / (glide * sampleRate); return; }
                v.releasing = true;
            }
        }
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr) { v = &voices[(size_t) next]; next = (next + 1) % (int) voices.size(); }
        *v = {};
        v->active = true; v->key = key; v->gain = vel;
        v->semis = (glide > 0.0f && lastSemis > -900.0 && (mono || slide)) ? lastSemis : target;
        v->targetSemis = target;
        v->glideRate = glide > 0.0f ? 1.0 / (glide * sampleRate) : 1.0;
        lastSemis = target;
    }
    void noteOff (int key) override
    {
        for (auto& v : voices) if (v.active && v.key == key && mono) v.releasing = true;
    }
    void allNotesOff() override { for (auto& v : voices) v.active = false; }
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
                if (v.releasing) { v.rel -= 1.0f / (0.01f * (float) sampleRate); if (v.rel <= 0.0f) { v.active = false; break; } g *= v.rel; }
                oL[i] += (l[i0] + f * (l[i0 + 1] - l[i0])) * g;
                oR[i] += (r[i0] + f * (r[i0 + 1] - r[i0])) * g;
                v.pos += std::pow (2.0, v.semis / 12.0);
            }
        }
    }
private:
    struct Voice { bool active = false, releasing = false; int key = 0; double pos = 0, semis = 0, targetSemis = 0, glideRate = 1; float gain = 1, rel = 1; };
    SharedSample sample;
    std::array<Voice, 16> voices {};
    int next = 0, rootKey = 60;
    double sampleRate = 44100.0, lastSemis = -999.0;
    float pitch = 0, glide = 0; bool mono = false;
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
                const float dt = (float) (hz / sampleRate);
                float s = osc (v.ph1, dt);
                if (p.detune != 0.0f || p.unison > 1) s = 0.5f * (s + osc (v.ph2, dt * detRatio));
                v.ph1 += dt; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                v.ph2 += dt * detRatio; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                if (p.drive > 0.0f) s = std::tanh (s * (1.0f + p.drive * 8.0f)) / std::tanh (1.0f + p.drive * 8.0f) ;
                // TPT state-variable lowpass
                const float v3 = s - v.ic2, v1 = a1 * v.ic1 + a2 * v3, v2 = v.ic2 + a2 * v.ic1 + a3 * v3;
                v.ic1 = 2.0f * v1 - v.ic1; v.ic2 = 2.0f * v2 - v.ic2;
                const float y = v2 * v.env * v.velocity * 0.5f;
                oL[i] += y; oR[i] += y;
                ++v.age;
            }
        }
    }
private:
    struct Voice { bool active = false; int key = 0, stage = 0; double current = 60, target = 60, glideRate = 1, age = 0;
                   float env = 0, velocity = 1, ph1 = 0, ph2 = 0.25f, ic1 = 0, ic2 = 0; };
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
    std::array<Voice, 12> voices {};
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
    {
        juce::LagrangeInterpolator li;
        li.process (ratio, raw.getReadPointer (ch), res.getWritePointer (ch), outLen);
    }
    return std::make_shared<const juce::AudioBuffer<float>> (std::move (res));
}
}
