// "Low-End Anchor": M/S processing for the bass bus.
//
//  * Mono Lock — 12 dB/oct Butterworth high-pass at 130 Hz on the Side signal
//    only. Everything below it collapses to the centre; the Mid path is
//    untouched so the low end itself is never filtered.
//  * Grit — the 200..800 Hz band of the Mid signal is split off with
//    minimum-phase filters, pushed through an odd-harmonic wave-shaper and
//    put back in place of the clean band.

#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace rockglue
{

class BassAnchor
{
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        sideHpf.setCoefficients(BiquadCoefficients::highPass(fs, kMonoLockHz));
        // 4th-order low edge (two cascaded Butterworth stages) keeps the sub
        // fundamentals below 200 Hz out of the shaper.
        bandHpf1.setCoefficients(BiquadCoefficients::highPass(fs, kGritLowHz));
        bandHpf2.setCoefficients(BiquadCoefficients::highPass(fs, kGritLowHz));
        bandLpf.setCoefficients(BiquadCoefficients::lowPass(fs, kGritHighHz));
        reset();
    }

    void reset()
    {
        sideHpf.reset();
        bandHpf1.reset();
        bandHpf2.reset();
        bandLpf.reset();
    }

    void setMonoLock(bool on) { monoLock = on; }
    void setGrit(float grit01) { grit = std::clamp(grit01, 0.0f, 1.0f); }

    inline void processSample(float& l, float& r) noexcept
    {
        float mid  = 0.5f * (l + r);
        float side = 0.5f * (l - r);

        // Keep the filter state running even when bypassed so toggling Mono
        // Lock never clicks.
        const float filteredSide = sideHpf.process(side);
        if (monoLock)
            side = filteredSide;

        // Band filters always run so the Grit control can be swept from zero
        // without releasing stale filter state.
        const float band = bandLpf.process(bandHpf2.process(bandHpf1.process(mid)));
        if (grit > 0.0f)
        {
            const float drive = 1.0f + grit * 5.0f;
            // Small-signal gain of the shaped band is (1 + grit/4): quiet
            // passages keep their tone, hot notes get the harmonics.
            const float shaped = shapeOdd(band * drive) / drive * (1.0f + grit * 0.25f);
            mid += grit * (shaped - band);
        }

        l = mid + side;
        r = mid - side;
    }

    static constexpr double kMonoLockHz = 130.0;
    static constexpr double kGritLowHz  = 200.0;
    static constexpr double kGritHighHz = 800.0;

private:
    // Symmetric (odd-order only) soft clipper: tanh with a cubic pre-bend for
    // a tape-ish 3rd/5th harmonic bias. Memoryless, so zero latency.
    static inline float shapeOdd(float x) noexcept
    {
        const float bent = x + 0.15f * x * x * x;
        return std::tanh(bent);
    }

    double fs = 48000.0;
    Biquad sideHpf, bandHpf1, bandHpf2, bandLpf;
    bool monoLock = true;
    float grit = 0.0f;
};

} // namespace rockglue
