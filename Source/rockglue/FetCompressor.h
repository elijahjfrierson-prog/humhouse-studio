// "Smasher": 1176-style FET compressor for the drum bus.
//
// The 1176 is a feedback design: the gain element sits before the detector,
// so the detector sees the already-compressed output. That topology is what
// gives it the fast, grabby, program-dependent feel. We model it sample by
// sample (the feedback path is one sample long) so there is no look-ahead and
// no block delay. Attack 0.05 ms, release 50 ms, ratio 4:1, all fixed.

#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace rockglue
{

class FetCompressor
{
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        attackCoeff  = timeConstantCoefficient(kAttackMs, fs);
        releaseCoeff = timeConstantCoefficient(kReleaseMs, fs);
        // The gain-cell control voltage is smoothed by the FET's own RC, which
        // is what stops the ultra-fast attack turning into pure distortion.
        controlCoeff = timeConstantCoefficient(0.02, fs);
        reset();
    }

    void reset()
    {
        envelope = 0.0f;
        gainLin = 1.0f;
        currentGr = 0.0f;
    }

    // Input gain in dB (0..+30). More drive = harder into the threshold and
    // the output-stage saturation.
    void setDriveDb(float db) { driveGain = dbToGain(db); driveDb = db; }
    void setMix(float mix01) { mix = std::clamp(mix01, 0.0f, 1.0f); }

    // Gain reduction of the most recent sample, in dB (positive number).
    float getGainReductionDb() const noexcept { return currentGr; }

    // Dry/wet is applied inside so the parallel blend is phase-coherent (both
    // paths are the same length: zero samples).
    inline void processSample(float& l, float& r) noexcept
    {
        const float dryL = l, dryR = r;

        // --- gain cell (feedback: uses the gain computed from the previous sample)
        const float xl = l * driveGain;
        const float xr = r * driveGain;
        float yl = xl * gainLin;
        float yr = xr * gainLin;

        // --- detector: stereo-linked peak, sees the *output* of the gain cell
        const float peak = std::max(std::abs(yl), std::abs(yr));
        const float coeff = peak > envelope ? attackCoeff : releaseCoeff;
        envelope = peak + coeff * (envelope - peak);

        const float envDb = gainToDb(envelope);
        const float over = envDb - kThresholdDb;
        // In a feedback design the detector sees the already-reduced output,
        // so the loop needs (ratio - 1) dB of reduction per dB of overshoot
        // for the input/output slope to settle at the nominal ratio.
        float grDb = 0.0f;
        if (over > kKneeDb * 0.5f)
            grDb = over * (kRatio - 1.0f);
        else if (over > -kKneeDb * 0.5f)
        {
            const float t = over + kKneeDb * 0.5f;
            grDb = (kRatio - 1.0f) * t * t / (2.0f * kKneeDb);
        }

        // Feedback compressors converge on the reduction rather than hitting
        // it directly; the RC on the control voltage models the FET response.
        const float targetGain = dbToGain(-grDb);
        gainLin = targetGain + controlCoeff * (gainLin - targetGain);
        currentGr = -gainToDb(gainLin);

        // --- output stage: class-A transformer-coupled, slight asymmetry
        yl = saturate(yl);
        yr = saturate(yr);

        // Auto make-up: pull back a share of the drive so the wet path stays
        // in the same ballpark as the dry one at any drive setting.
        const float makeup = dbToGain(-driveDb * 0.6f);
        yl *= makeup;
        yr *= makeup;

        l = dryL + mix * (yl - dryL);
        r = dryR + mix * (yr - dryR);
    }

    static constexpr float kAttackMs   = 0.05f;
    static constexpr float kReleaseMs  = 50.0f;
    static constexpr float kRatio      = 4.0f;
    static constexpr float kThresholdDb = -18.0f;
    static constexpr float kKneeDb     = 3.0f;

private:
    static inline float saturate(float x) noexcept
    {
        // Odd tanh core with a touch of even-order asymmetry, memoryless.
        const float asym = 0.06f * x * x;
        const float y = std::tanh(x + asym) - std::tanh(asym);
        return y;
    }

    double fs = 48000.0;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f, controlCoeff = 0.0f;
    float envelope = 0.0f;
    float gainLin = 1.0f;
    float currentGr = 0.0f;
    float driveGain = 1.0f, driveDb = 0.0f;
    float mix = 0.5f;
};

} // namespace rockglue
