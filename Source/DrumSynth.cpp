#include "DrumSynth.h"
#include <random>
#include <cmath>

namespace hhs
{
const char* builtinDrumName (int i)
{
    static const char* n[] { "Kick", "Clap", "Snare", "Hat", "Open Hat", "Perc", "Rim", "Tom" };
    return n[juce::jlimit (0, NumBuiltinDrums - 1, i)];
}

juce::AudioBuffer<float> renderBuiltinDrum (int which, double sr)
{
    const double lens[] { 0.6, 0.35, 0.3, 0.08, 0.45, 0.25, 0.08, 0.5 };
    const int n = (int) (lens[juce::jlimit (0, NumBuiltinDrums - 1, which)] * sr);
    juce::AudioBuffer<float> b (2, n);
    auto* d = b.getWritePointer (0);
    std::mt19937 rng (1234u + (unsigned) which);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    double phase = 0.0;
    float hp = 0.0f, lastN = 0.0f, bp1 = 0.0f, bp2 = 0.0f;
    const double twoPi = juce::MathConstants<double>::twoPi;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        float s = 0.0f;
        const float noise = u (rng);
        switch (which)
        {
            case Kick:
            {
                const double f = 45.0 + 110.0 * std::exp (-t * 30.0);
                phase += twoPi * f / sr;
                s = (float) (std::sin (phase) * std::exp (-t * 6.0));
                s += noise * 0.15f * (float) std::exp (-t * 300.0);
                s = std::tanh (s * 1.6f);
                break;
            }
            case Clap:
            {
                float env = 0.0f;
                for (double o : { 0.0, 0.011, 0.022 }) if (t >= o) env = std::max (env, (float) std::exp (-(t - o) * 180.0));
                if (t >= 0.03) env = std::max (env, 0.7f * (float) std::exp (-(t - 0.03) * 18.0));
                bp1 += 0.35f * (noise - bp1); bp2 += 0.35f * (bp1 - bp2);
                s = (bp1 - bp2) * env * 2.2f;
                break;
            }
            case Snare:
            {
                phase += twoPi * (180.0 + 60.0 * std::exp (-t * 40.0)) / sr;
                hp = 0.6f * (hp + noise - lastN); lastN = noise;
                s = (float) (std::sin (phase) * std::exp (-t * 25.0)) * 0.6f + hp * (float) std::exp (-t * 16.0) * 0.7f;
                break;
            }
            case Hat: case OpenHat:
            {
                hp = 0.3f * (hp + noise - lastN); lastN = noise;
                s = hp * (float) std::exp (-t * (which == Hat ? 70.0 : 9.0)) * 1.4f;
                break;
            }
            case Perc:
            {
                phase += twoPi * 620.0 / sr;
                s = (float) (std::sin (phase) * std::exp (-t * 22.0));
                break;
            }
            case Rim:
            {
                phase += twoPi * 1700.0 / sr;
                s = (float) (std::sin (phase) * std::exp (-t * 90.0)) + noise * 0.3f * (float) std::exp (-t * 200.0);
                break;
            }
            default:
            {
                phase += twoPi * (110.0 + 60.0 * std::exp (-t * 20.0)) / sr;
                s = (float) (std::sin (phase) * std::exp (-t * 8.0));
                break;
            }
        }
        d[i] = s * 0.9f;
    }
    // short fade-out to avoid clicks
    const int fade = std::min (n, (int) (0.005 * sr));
    b.applyGainRamp (0, n - fade, fade, 1.0f, 0.0f);
    b.copyFrom (1, 0, b, 0, 0, n);
    return b;
}
}
