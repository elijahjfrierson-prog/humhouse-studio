// "The Pocket": complementary guitar/vocal interlock.
//
// One minimum-phase bell at 2.5 kHz, Q = 1.0, applied as a cut to the guitar
// bus and the same magnitude boost to the vocal bus. "Carve Pocket" scales the
// depth 0..100 % -> 0..2 dB. Because both are minimum-phase biquads of equal Q
// and opposite gain, their phase shifts are near-complementary and the summed
// result stays coherent.
//
// On a finished stereo mix the stems aren't separable, so the same pair is
// applied Mid/Side: boost on Mid (where the vocal sits), cut on Side (where
// double-tracked guitars live).

#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace rockglue
{

class PocketEq
{
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        lastDepth = -1.0f;
        setDepth(depth);
        reset();
    }

    void reset()
    {
        guitarBell.reset();
        vocalBell.reset();
        sideBell.reset();
        midBell.reset();
    }

    // 0..1 -> 0..kMaxDb. Coefficients are only recalculated when the value
    // actually moves, so this is cheap to call per block.
    void setDepth(float depth01)
    {
        depth = std::clamp(depth01, 0.0f, 1.0f);
        if (std::abs(depth - lastDepth) < 1.0e-6f)
            return;
        lastDepth = depth;
        const double gainDb = depth * kMaxDb;
        const auto cut = BiquadCoefficients::peak(fs, kCentreHz, kQ, -gainDb);
        const auto boost = BiquadCoefficients::peak(fs, kCentreHz, kQ, gainDb);
        guitarBell.setCoefficients(cut);
        vocalBell.setCoefficients(boost);
        sideBell.setCoefficients(cut);
        midBell.setCoefficients(boost);
    }

    inline void processGuitar(float& l, float& r) noexcept { guitarBell.process(l, r); }
    inline void processVocal(float& l, float& r) noexcept  { vocalBell.process(l, r); }

    // Master-bus form: vocal boost on Mid, guitar cut on Side.
    inline void processMix(float& l, float& r) noexcept
    {
        float mid = 0.5f * (l + r), side = 0.5f * (l - r);
        mid = midBell.process(mid);
        side = sideBell.process(side);
        l = mid + side;
        r = mid - side;
    }

    static constexpr double kCentreHz = 2500.0;
    static constexpr double kQ        = 1.0;
    static constexpr double kMaxDb    = 2.0;

private:
    double fs = 48000.0;
    float depth = 1.0f, lastDepth = -1.0f;
    StereoBiquad guitarBell, vocalBell;
    Biquad sideBell, midBell;
};

} // namespace rockglue
