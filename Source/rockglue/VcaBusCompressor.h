// "The VCA Glue": SSL G-series style feed-forward stereo bus compressor.
//
// Detector is a stereo-linked RMS-ish level with 30 ms attack, 100 ms release
// (or program-dependent auto release), 2:1 ratio and a 6 dB soft knee. The
// gain computer runs on the current sample only: no look-ahead, no delay.

#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace rockglue
{

class VcaBusCompressor
{
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        attackCoeff      = timeConstantCoefficient(kAttackMs, fs);
        releaseCoeff     = timeConstantCoefficient(kReleaseMs, fs);
        autoFastCoeff    = timeConstantCoefficient(kAutoFastMs, fs);
        autoSlowCoeff    = timeConstantCoefficient(kAutoSlowMs, fs);
        reset();
    }

    void reset()
    {
        envelopeDb = -120.0f;
        autoSlowState = 0.0f;
        currentGr = 0.0f;
    }

    void setThresholdDb(float db) { thresholdDb = db; }
    void setMakeupDb(float db)    { makeupGain = dbToGain(db); }
    void setAutoRelease(bool on)  { autoRelease = on; }

    float getGainReductionDb() const noexcept { return currentGr; }

    inline void processSample(float& l, float& r) noexcept
    {
        // --- side-chain: stereo-linked, sum of squares approximates the SSL's
        // shared detector so the image never wanders.
        const float level = std::sqrt(0.5f * (l * l + r * r));
        const float levelDb = gainToDb(level);

        // --- gain computer with soft knee
        const float over = levelDb - thresholdDb;
        float grTargetDb = 0.0f;
        if (over > kKneeDb * 0.5f)
            grTargetDb = over * (1.0f - 1.0f / kRatio);
        else if (over > -kKneeDb * 0.5f)
        {
            const float t = over + kKneeDb * 0.5f;
            grTargetDb = (1.0f - 1.0f / kRatio) * t * t / (2.0f * kKneeDb);
        }

        // Slow integrator tracks how hard the compressor has been working,
        // during attack as well as release, so program-dependent memory builds
        // up on sustained material.
        autoSlowState = grTargetDb + autoSlowCoeff * (autoSlowState - grTargetDb);

        // --- ballistics in the log domain (smooth attack, musical release)
        if (grTargetDb > currentGr)
        {
            currentGr = grTargetDb + attackCoeff * (currentGr - grTargetDb);
        }
        else if (autoRelease)
        {
            // Two-stage auto release: a fast stage handles transients while the
            // slow memory holds the release open after sustained compression.
            const float fastTarget = std::max(grTargetDb, autoSlowState);
            currentGr = fastTarget + autoFastCoeff * (currentGr - fastTarget);
        }
        else
        {
            currentGr = grTargetDb + releaseCoeff * (currentGr - grTargetDb);
        }

        const float gain = dbToGain(-currentGr) * makeupGain;
        l *= gain;
        r *= gain;
    }

    static constexpr float kAttackMs  = 30.0f;
    static constexpr float kReleaseMs = 100.0f;
    static constexpr float kAutoFastMs = 80.0f;
    static constexpr float kAutoSlowMs = 1200.0f;
    static constexpr float kRatio     = 2.0f;
    static constexpr float kKneeDb    = 6.0f;

private:
    double fs = 48000.0;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f, autoFastCoeff = 0.0f, autoSlowCoeff = 0.0f;
    float envelopeDb = -120.0f;
    float autoSlowState = 0.0f;
    float currentGr = 0.0f;
    float thresholdDb = -10.0f;
    float makeupGain = 1.0f;
    bool autoRelease = true;
};

} // namespace rockglue
