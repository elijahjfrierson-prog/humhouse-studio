#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace hhir
{

// HUM HOUSE ISOROOM — zero-latency vocal isolation.
//
// Two stages, both strictly sample-by-sample so there is no lookahead and no
// block delay:
//
//   1. A serial Linkwitz-Riley crossover chain splits the input into kBands
//      bands. Their sum is an all-pass of the input, so with every band gain
//      at 1.0 the plug-in is transparent. Each band has its own noise-floor
//      estimate (learned on demand, or tracked continuously with minimum
//      statistics) and its own downward expander; a band that sits at its noise
//      floor is pulled down by up to `depth` dB, a band the voice is driving
//      passes untouched. This is what takes the room out from *around* the voice
//      while the singer is singing.
//
//   2. A broadband voice gate keyed from the vocal band (a band-passed copy of
//      the input, so a hi-hat or a door slam does not open it) with a fast
//      attack, hold and release. This is what takes the room out *between*
//      phrases.
//
// Deliberately free of JUCE so the DSP can be tested as a plain executable.

constexpr int kBands = 12;

float dbToGain(float db) noexcept;
float gainToDb(float gain) noexcept;

// Transposed direct-form II biquad.
class Biquad
{
public:
    void setLowPass(double sampleRate, double hz, double q) noexcept;
    void setHighPass(double sampleRate, double hz, double q) noexcept;
    void setBandPass(double sampleRate, double hz, double q) noexcept;
    void setAllPass(double sampleRate, double hz, double q) noexcept;
    void reset() noexcept { z1 = z2 = 0.0f; }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

// 4th-order Linkwitz-Riley: two cascaded Butterworth 2nd-order sections. The
// low and high outputs sum to a 2nd-order all-pass at the crossover, which is
// what lets the band tree reconstruct the input exactly in magnitude: every
// band below a crossover is run through that crossover's all-pass before the
// bands are summed.
class LinkwitzRiley
{
public:
    void prepare(double sampleRate, double hz) noexcept;
    void reset() noexcept;
    void process(float x, float& low, float& high) noexcept
    {
        low = lp2.process(lp1.process(x));
        high = hp2.process(hp1.process(x));
    }
    float allPass(float x) noexcept { return ap.process(x); }

private:
    Biquad lp1, lp2, hp1, hp2, ap;
};

// Peak/RMS-ish envelope follower with separate attack and release, in dB.
class Envelope
{
public:
    void prepare(double sampleRate) noexcept { sr = sampleRate; }
    void setTimes(double attackMs, double releaseMs) noexcept;
    void reset(float level = 0.0f) noexcept { env = level; }

    float process(float x) noexcept
    {
        const float a = x * x;
        env += (a > env ? attackCoef : releaseCoef) * (a - env);
        return env;
    }

    float value() const noexcept { return env; }

private:
    double sr = 48000.0;
    float attackCoef = 1.0f, releaseCoef = 1.0f;
    float env = 0.0f;
};

struct IsoParameters
{
    // Voice gate
    float thresholdDb = -40.0f;   // gate opens when the key exceeds this
    float attackMs = 0.5f;
    float holdMs = 80.0f;
    float releaseMs = 120.0f;
    float rangeDb = -80.0f;       // how far the gate closes (0 = off, -80 = silence)
    float hysteresisDb = 6.0f;

    // Room suppression
    float depthDb = 24.0f;        // max per-band attenuation of what sits at the noise floor
    float sensitivity = 0.5f;     // 0..1: how far above the noise floor a band must rise to be voice
    float smoothingMs = 8.0f;     // per-band gain smoothing
    bool autoTrack = true;        // keep re-learning the floor from the quiet moments
    bool learning = false;        // explicit "Learn room" capture

    // Tone
    float lowCutHz = 80.0f;
    float highCutHz = 16000.0f;
    float keyLowHz = 180.0f;      // vocal key band
    float keyHighHz = 5000.0f;

    // Output
    float mix = 1.0f;             // 0 = dry, 1 = fully isolated
    float outputDb = 0.0f;
    bool listenResidual = false;  // monitor what was removed
};

class IsoRoomEngine
{
public:
    IsoRoomEngine();

    void prepare(double sampleRate, int maxBlock);
    void reset();

    void setParameters(const IsoParameters& p) noexcept;
    const IsoParameters& parameters() const noexcept { return params; }

    // Forget the learned room, start from an unknown floor.
    void resetNoiseProfile() noexcept;

    // In-place, `numChannels` <= 2, all channels share one detector so a stereo
    // pair never drifts apart in the image.
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    // Metering (main thread reads, atomics are overkill for a 25 Hz UI).
    float gateGainDb() const noexcept { return meterGateDb; }
    float suppressionDb() const noexcept { return meterSuppressDb; }
    float inputDb() const noexcept { return meterInputDb; }
    float noiseFloorDb(int band) const noexcept { return floorDb[static_cast<size_t>(band)]; }
    bool hasNoiseProfile() const noexcept { return profileValid; }
    double latencySamples() const noexcept { return 0.0; }

    static double bandEdgeHz(int index) noexcept;  // index 0..kBands-2

private:
    struct Band
    {
        Envelope env;
        float gain = 1.0f;
        float slowDb = -140.0f;   // 200 ms view of the band level, what the floor tracker watches
    };

    void updateCoefficients() noexcept;
    float computeBandGain(int band, Band& state, float energy) noexcept;

    double sr = 48000.0;
    IsoParameters params;

    std::array<LinkwitzRiley, kBands - 1> splitters[2];
    std::array<Band, kBands> bands[2];
    std::array<float, kBands> floorDb {};        // learned floor per band, in dB (energy)
    std::array<float, kBands> minTrack {};       // minimum-statistics tracker
    std::array<float, kBands> learnAccum {};
    std::array<float, kBands> bandTarget {};
    int learnSamples = 0;
    bool profileValid = false;

    Biquad lowCut[2], highCut[2];
    Biquad keyLow, keyHigh;
    Envelope keyEnv;

    float gateGain = 0.0f;
    float gateTarget = 0.0f;
    bool gateOpen = false;
    int holdCounter = 0;
    float gateAttackCoef = 1.0f, gateReleaseCoef = 1.0f;
    float bandSmoothCoef = 1.0f;
    float minRiseDbPerSample = 0.0f, minFallCoef = 1.0f, slowCoef = 1.0f;
    bool minValid = false;
    int samplesSeen = 0;
    int warmupSamples = 12000;

    float meterGateDb = -80.0f;
    float meterSuppressDb = 0.0f;
    float meterInputDb = -80.0f;

};

} // namespace hhir
