// JUCE-free core: owns the four bus nodes and the master glue stage and does
// the summing. The plug-in processor is a thin adapter over this so the whole
// signal path can be verified as a plain executable (see tests/).
//
//   Drums  -> Smasher (FET, parallel)         --.
//   Bass   -> Low-End Anchor (M/S HPF + grit) ---.
//   Guitars-> Pocket cut  (-2 dB @ 2.5k)       ----> sum -> VCA Glue -> out
//   Vocals -> Pocket boost (+2 dB @ 2.5k)     --/
//
// Master mode (the default, sitting on the 2-bus) runs the stereo mix through
// every node in series: Anchor (mono-locked lows, mid grit) -> Pocket in its
// Mid/Side form (+2 dB @ 2.5k on Mid, -2 dB on Side) -> Smasher as a parallel
// mix-bus smash -> Glue.

#pragma once

#include "BassAnchor.h"
#include "FetCompressor.h"
#include "PocketEq.h"
#include "VcaBusCompressor.h"

#include <algorithm>
#include <cmath>

namespace rockglue
{

struct StereoBlock
{
    float* left = nullptr;
    float* right = nullptr;
};

struct ConstStereoBlock
{
    const float* left = nullptr;
    const float* right = nullptr;
    bool valid() const noexcept { return left != nullptr && right != nullptr; }
};

// Snapshot of what the UI draws. Written by the audio thread once per block.
struct MeterFrame
{
    float drumGrDb = 0.0f;     // Smasher gain reduction (positive dB)
    float glueGrDb = 0.0f;     // VCA gain reduction (positive dB)
    float width = 0.0f;        // 0 = mono, 1 = fully wide (side/mid energy)
    float correlation = 1.0f;  // -1..+1 stereo correlation of the output
    float outPeakDb = -100.0f;
};

class RockGlueEngine
{
public:
    static constexpr int kLatencySamples = 0;

    void prepare(double sampleRate)
    {
        smasher.prepare(sampleRate);
        anchor.prepare(sampleRate);
        pocket.prepare(sampleRate);
        glue.prepare(sampleRate);
    }

    void reset()
    {
        smasher.reset();
        anchor.reset();
        pocket.reset();
        glue.reset();
    }

    // --- parameters (call from the audio thread before processing a block)
    void setDrumDriveDb(float db)   { smasher.setDriveDb(db); }
    void setDrumParallelMix(float m){ smasher.setMix(m); }
    void setMonoLock(bool on)       { anchor.setMonoLock(on); }
    void setGrit(float g)           { anchor.setGrit(g); }
    void setCarvePocket(float d)    { pocket.setDepth(d); }
    void setGlueThresholdDb(float t){ glue.setThresholdDb(t); }
    void setGlueMakeupDb(float m)   { glue.setMakeupDb(m); }
    void setGlueAutoRelease(bool a) { glue.setAutoRelease(a); }

    // Four discrete buses -> out. Any bus whose block is not valid is treated
    // as silent. `out` may alias `drums`.
    void processFourBus(ConstStereoBlock drums, ConstStereoBlock bass,
                        ConstStereoBlock guitars, ConstStereoBlock vocals,
                        StereoBlock out, int numSamples) noexcept
    {
        float peakDrumGr = 0.0f, peakGlueGr = 0.0f;
        double midE = 0.0, sideE = 0.0, lr = 0.0, ll = 0.0, rr = 0.0;
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float sumL = 0.0f, sumR = 0.0f;

            if (drums.valid())
            {
                float l = drums.left[i], r = drums.right[i];
                smasher.processSample(l, r);
                sumL += l; sumR += r;
                peakDrumGr = std::max(peakDrumGr, smasher.getGainReductionDb());
            }
            if (bass.valid())
            {
                float l = bass.left[i], r = bass.right[i];
                anchor.processSample(l, r);
                sumL += l; sumR += r;
            }
            if (guitars.valid())
            {
                float l = guitars.left[i], r = guitars.right[i];
                pocket.processGuitar(l, r);
                sumL += l; sumR += r;
            }
            if (vocals.valid())
            {
                float l = vocals.left[i], r = vocals.right[i];
                pocket.processVocal(l, r);
                sumL += l; sumR += r;
            }

            glue.processSample(sumL, sumR);
            peakGlueGr = std::max(peakGlueGr, glue.getGainReductionDb());

            out.left[i] = sumL;
            out.right[i] = sumR;
            accumulate(sumL, sumR, midE, sideE, ll, rr, lr, peak);
        }

        publish(peakDrumGr, peakGlueGr, midE, sideE, ll, rr, lr, peak);
    }

    // Master-bus mode: the stereo mix in place.
    void processMaster(StereoBlock io, int numSamples) noexcept
    {
        float peakDrumGr = 0.0f, peakGlueGr = 0.0f;
        double midE = 0.0, sideE = 0.0, lr = 0.0, ll = 0.0, rr = 0.0;
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = io.left[i], r = io.right[i];
            anchor.processSample(l, r);
            pocket.processMix(l, r);
            smasher.processSample(l, r);
            peakDrumGr = std::max(peakDrumGr, smasher.getGainReductionDb());
            glue.processSample(l, r);
            peakGlueGr = std::max(peakGlueGr, glue.getGainReductionDb());
            io.left[i] = l;
            io.right[i] = r;
            accumulate(l, r, midE, sideE, ll, rr, lr, peak);
        }

        publish(peakDrumGr, peakGlueGr, midE, sideE, ll, rr, lr, peak);
    }

    const MeterFrame& getMeterFrame() const noexcept { return meters; }

private:
    static inline void accumulate(float l, float r, double& midE, double& sideE,
                                  double& ll, double& rr, double& lr, float& peak) noexcept
    {
        const double m = 0.5 * (l + r), s = 0.5 * (l - r);
        midE += m * m; sideE += s * s;
        ll += static_cast<double>(l) * l; rr += static_cast<double>(r) * r; lr += static_cast<double>(l) * r;
        peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
    }

    void publish(float drumGr, float glueGr, double midE, double sideE,
                 double ll, double rr, double lr, float peak) noexcept
    {
        meters.drumGrDb = drumGr;
        meters.glueGrDb = glueGr;
        const double total = midE + sideE;
        meters.width = total > 1.0e-12 ? static_cast<float>(std::sqrt(sideE / total)) : 0.0f;
        const double denom = std::sqrt(ll * rr);
        meters.correlation = denom > 1.0e-12 ? static_cast<float>(lr / denom) : 1.0f;
        meters.outPeakDb = gainToDb(peak);
    }

    FetCompressor smasher;
    BassAnchor anchor;
    PocketEq pocket;
    VcaBusCompressor glue;
    MeterFrame meters;
};

} // namespace rockglue
