// Purify: one-knob "make it sound good" (our own Soundgoodizer-style processor). 3-band compression + tone + glow, 0 latency.
#include "Effects.h"
#include <cmath>

namespace hhs
{
namespace
{
struct Purify : BuiltinEffect
{
    Purify() { initDefaults(); }
    const char* typeId() const override { return "purify"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> d {
            { "amount", "Purify", 0, 1, 0.5f },
            { "mode", "Mode", 0, 4, 0, { "A  Warm Glue", "B  Bright Air", "C  Punch", "D  Wide Polish", "E  Loud Master" } } };
        return d;
    }
    void prepare (double s, int) override { sr = s; reset(); }
    void reset() override { for (auto& c : st) c = {}; }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        const float amt = juce::jlimit (0.0f, 1.0f, p (0));
        if (amt < 0.001f) return;
        const int mode = juce::jlimit (0, 4, (int) p (1));
        //                         low   mid   high  drive  width ratio
        static const float M[5][6] { { 1.5f, 0.0f,-0.5f, 0.35f, 0.0f, 3.0f },
                                     {-0.5f, 0.3f, 3.0f, 0.15f, 0.1f, 2.5f },
                                     { 2.0f, 0.8f, 1.0f, 0.30f, 0.0f, 4.0f },
                                     { 0.8f, 0.0f, 1.8f, 0.15f, 0.35f, 2.5f },
                                     { 1.5f, 0.6f, 1.5f, 0.55f, 0.15f, 6.0f } };
        const auto& m = M[mode];
        const float aLo = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 200.0f / (float) sr);
        const float aHi = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 3500.0f / (float) sr);
        const float att = 1.0f - std::exp (-1.0f / (0.005f * (float) sr)), rel = 1.0f - std::exp (-1.0f / (0.12f * (float) sr));
        const float ratio = 1.0f + (m[5] - 1.0f) * amt, thr = juce::Decibels::decibelsToGain (-12.0f - 12.0f * amt);
        const float bandG[3] { juce::Decibels::decibelsToGain (m[0] * amt * 2.0f), juce::Decibels::decibelsToGain (m[1] * amt * 2.0f), juce::Decibels::decibelsToGain (m[2] * amt * 2.0f) };
        const float drive = 1.0f + 3.0f * m[3] * amt, makeup = juce::Decibels::decibelsToGain (4.0f * amt);
        const int nch = juce::jmin (2, b.getNumChannels());
        for (int i = 0; i < n; ++i)
        {
            float y[2] {};
            for (int ch = 0; ch < nch; ++ch)
            {
                auto& c = st[(size_t) ch];
                const float x = b.getSample (ch, i);
                c.lp1 += aLo * (x - c.lp1); c.lp2 += aHi * (x - c.lp2);
                float band[3] { c.lp1, c.lp2 - c.lp1, x - c.lp2 };   // low + mid + high = x
                float out = 0;
                for (int k = 0; k < 3; ++k)
                {
                    const float lv = std::abs (band[k]);
                    c.env[k] += (lv > c.env[k] ? att : rel) * (lv - c.env[k]);
                    float g = 1.0f;
                    if (c.env[k] > thr) g = std::pow (c.env[k] / thr, 1.0f / ratio - 1.0f);
                    out += band[k] * g * bandG[k];
                }
                out *= makeup;
                const float wet = std::tanh (out * drive) / std::tanh (drive);   // glow, unity at small levels' slope
                y[ch] = juce::jlimit (-0.98f, 0.98f, x + (wet - x) * amt + (out - x) * 0.0f);
            }
            if (nch == 2 && m[4] > 0)
            {
                const float mid = 0.5f * (y[0] + y[1]), side = 0.5f * (y[0] - y[1]) * (1.0f + m[4] * amt);
                y[0] = mid + side; y[1] = mid - side;
            }
            for (int ch = 0; ch < nch; ++ch) b.setSample (ch, i, y[ch]);
        }
    }
    struct Ch { float lp1 = 0, lp2 = 0; float env[3] {}; };
    std::array<Ch, 2> st {};
    double sr = 44100;
};
}
std::unique_ptr<BuiltinEffect> createPurify() { return std::make_unique<Purify>(); }
}
