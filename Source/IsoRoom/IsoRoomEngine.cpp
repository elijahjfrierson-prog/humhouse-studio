#include "IsoRoomEngine.h"

#include <algorithm>
#include <cmath>

namespace hhir
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTiny = 1.0e-12f;       // keeps log10 finite and kills denormals
constexpr float kFloorBiasDb = 3.0f;     // a minimum tracker sits below the mean by about this much
constexpr float kKneeDb = 12.0f;         // width of the band expander's transition
constexpr float kUnknownFloorDb = -140.0f;

float energyToDb(float energy) noexcept
{
    return 10.0f * std::log10(2.0f * energy + kTiny);   // full-scale sine reads 0 dBFS
}

float onePoleCoef(double sampleRate, double ms) noexcept
{
    if (ms <= 0.0)
        return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * ms * 0.001)));
}

// Log-spaced band edges. Everything a voice does lives between the first and
// the last; the outer bands catch rumble and air.
constexpr double kEdges[kBands - 1] = { 120.0, 200.0, 320.0, 500.0, 800.0, 1250.0,
                                        2000.0, 3200.0, 5000.0, 8000.0, 12000.0 };
} // namespace

float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
float gainToDb(float gain) noexcept { return 20.0f * std::log10(std::max(gain, 1.0e-7f)); }

// --- Biquad -----------------------------------------------------------------

void Biquad::setLowPass(double sampleRate, double hz, double q) noexcept
{
    hz = std::clamp(hz, 10.0, sampleRate * 0.45);
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 - cs) * 0.5 / a0);
    b1 = static_cast<float>((1.0 - cs) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * cs / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

void Biquad::setHighPass(double sampleRate, double hz, double q) noexcept
{
    hz = std::clamp(hz, 10.0, sampleRate * 0.45);
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 + cs) * 0.5 / a0);
    b1 = static_cast<float>(-(1.0 + cs) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * cs / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

void Biquad::setBandPass(double sampleRate, double hz, double q) noexcept
{
    hz = std::clamp(hz, 10.0, sampleRate * 0.45);
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>(alpha / a0);
    b1 = 0.0f;
    b2 = static_cast<float>(-alpha / a0);
    a1 = static_cast<float>(-2.0 * cs / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

void Biquad::setAllPass(double sampleRate, double hz, double q) noexcept
{
    hz = std::clamp(hz, 10.0, sampleRate * 0.45);
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 - alpha) / a0);
    b1 = static_cast<float>(-2.0 * cs / a0);
    b2 = 1.0f;
    a1 = b1;
    a2 = b0;
}

// --- Linkwitz-Riley -----------------------------------------------------------

void LinkwitzRiley::prepare(double sampleRate, double hz) noexcept
{
    const double q = 1.0 / std::sqrt(2.0);
    lp1.setLowPass(sampleRate, hz, q);
    lp2.setLowPass(sampleRate, hz, q);
    hp1.setHighPass(sampleRate, hz, q);
    hp2.setHighPass(sampleRate, hz, q);
    ap.setAllPass(sampleRate, hz, q);
}

void LinkwitzRiley::reset() noexcept
{
    lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); ap.reset();
}

// --- Envelope -----------------------------------------------------------------

void Envelope::setTimes(double attackMs, double releaseMs) noexcept
{
    attackCoef = onePoleCoef(sr, attackMs);
    releaseCoef = onePoleCoef(sr, releaseMs);
}

// --- Engine -------------------------------------------------------------------

double IsoRoomEngine::bandEdgeHz(int index) noexcept
{
    index = std::clamp(index, 0, kBands - 2);
    return kEdges[index];
}

IsoRoomEngine::IsoRoomEngine()
{
    floorDb.fill(kUnknownFloorDb);
    minTrack.fill(0.0f);
}

void IsoRoomEngine::prepare(double sampleRate, int maxBlock)
{
    sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    (void) maxBlock;

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < kBands - 1; ++i)
            splitters[ch][static_cast<size_t>(i)].prepare(sr, kEdges[i]);
        for (auto& b : bands[ch])
        {
            b.env.prepare(sr);
            b.env.setTimes(1.5, 40.0);
        }
    }
    keyEnv.prepare(sr);
    keyEnv.setTimes(0.25, 30.0);

    // Minimum statistics: drop to a new low within ~60 ms, climb back at a
    // pace that only follows a change of room, never a held vowel.
    minRiseDbPerSample = static_cast<float>(4.0 / sr);
    minFallCoef = onePoleCoef(sr, 60.0);
    slowCoef = onePoleCoef(sr, 200.0);
    warmupSamples = static_cast<int>(sr * 0.25);

    updateCoefficients();
    reset();
}

void IsoRoomEngine::reset()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        for (auto& s : splitters[ch]) s.reset();
        for (auto& b : bands[ch]) { b.env.reset(); b.gain = 1.0f; b.slowDb = kUnknownFloorDb; }
        lowCut[ch].reset();
        highCut[ch].reset();
    }
    keyLow.reset();
    keyHigh.reset();
    keyEnv.reset();
    minValid = false;
    samplesSeen = 0;
    gateGain = dbToGain(params.rangeDb);
    gateTarget = gateGain;
    gateOpen = false;
    holdCounter = 0;
    meterGateDb = params.rangeDb;
    meterSuppressDb = 0.0f;
    meterInputDb = -80.0f;
}

void IsoRoomEngine::resetNoiseProfile() noexcept
{
    floorDb.fill(kUnknownFloorDb);
    minTrack.fill(0.0f);
    learnAccum.fill(0.0f);
    learnSamples = 0;
    profileValid = false;
    minValid = false;
    samplesSeen = 0;
}

void IsoRoomEngine::setParameters(const IsoParameters& p) noexcept
{
    const bool wasLearning = params.learning;
    params = p;
    params.thresholdDb = std::clamp(params.thresholdDb, -90.0f, 0.0f);
    params.rangeDb = std::clamp(params.rangeDb, -80.0f, 0.0f);
    params.depthDb = std::clamp(params.depthDb, 0.0f, 60.0f);
    params.sensitivity = std::clamp(params.sensitivity, 0.0f, 1.0f);
    params.mix = std::clamp(params.mix, 0.0f, 1.0f);
    params.highCutHz = std::max(params.highCutHz, params.lowCutHz + 50.0f);
    params.keyHighHz = std::max(params.keyHighHz, params.keyLowHz + 50.0f);

    if (params.learning && ! wasLearning)
    {
        learnAccum.fill(0.0f);
        learnSamples = 0;
    }
    else if (! params.learning && wasLearning && learnSamples > 0)
    {
        for (int b = 0; b < kBands; ++b)
            floorDb[static_cast<size_t>(b)] = learnAccum[static_cast<size_t>(b)] / static_cast<float>(learnSamples);
        profileValid = true;
    }

    updateCoefficients();
}

void IsoRoomEngine::updateCoefficients() noexcept
{
    gateAttackCoef = onePoleCoef(sr, params.attackMs);
    gateReleaseCoef = onePoleCoef(sr, params.releaseMs);
    bandSmoothCoef = onePoleCoef(sr, params.smoothingMs);

    for (int ch = 0; ch < 2; ++ch)
    {
        lowCut[ch].setHighPass(sr, params.lowCutHz, 0.7071);
        highCut[ch].setLowPass(sr, params.highCutHz, 0.7071);
    }
    keyLow.setHighPass(sr, params.keyLowHz, 0.7071);
    keyHigh.setLowPass(sr, params.keyHighHz, 0.7071);
}

float IsoRoomEngine::computeBandGain(int band, Band& state, float energy) noexcept
{
    const auto b = static_cast<size_t>(band);
    const float levelDb = energyToDb(energy);

    // Track the quiet moments. A 200 ms view of the band is fed to a minimum
    // tracker that falls quickly and rises slowly, so it settles on the room,
    // not on the singer.
    if (state.slowDb <= kUnknownFloorDb)
        state.slowDb = levelDb;
    else
        state.slowDb += slowCoef * (levelDb - state.slowDb);

    // The tracker climbs at a fixed few dB per second, so a held note lifts
    // it by a hair and a real change of room catches up within seconds.
    if (! minValid)
        minTrack[b] = state.slowDb;
    else if (state.slowDb < minTrack[b])
        minTrack[b] += minFallCoef * (state.slowDb - minTrack[b]);
    else
        minTrack[b] = std::min(minTrack[b] + minRiseDbPerSample, state.slowDb);

    if (params.learning)
        learnAccum[b] += levelDb;   // divided out when learning stops
    else if (params.autoTrack && minValid)
        floorDb[b] = minTrack[b] + kFloorBiasDb;

    if (params.depthDb <= 0.0f || floorDb[b] <= kUnknownFloorDb + 1.0f)
        return 1.0f;

    // A band is "voice" once it clears the floor by the margin; the knee eases
    // the hand-off so a fading consonant does not flicker.
    const float marginDb = 3.0f + 15.0f * params.sensitivity;
    const float over = levelDb - (floorDb[b] + marginDb);
    const float closed = std::clamp(-over / kKneeDb, 0.0f, 1.0f);
    return dbToGain(-params.depthDb * closed * closed);
}

void IsoRoomEngine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::clamp(numChannels, 0, 2);
    if (numChannels == 0 || numSamples <= 0)
        return;

    const float openGain = 1.0f;
    const float closedGain = dbToGain(params.rangeDb);
    const int holdSamples = static_cast<int>(params.holdMs * 0.001f * sr);
    const float outGain = dbToGain(params.outputDb);
    const float wetMix = params.mix;
    const float dryMix = 1.0f - wetMix;
    const float invChannels = 1.0f / static_cast<float>(numChannels);

    float inputPeakEnergy = 0.0f;
    double suppressAccum = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // ---- voice key: mono sum through the vocal band ----
        float mono = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            mono += channels[ch][i];
        mono *= invChannels;

        const float key = keyHigh.process(keyLow.process(mono));
        const float keyEnergy = keyEnv.process(key);
        inputPeakEnergy = std::max(inputPeakEnergy, keyEnergy);
        const float keyDb = energyToDb(keyEnergy);

        if (keyDb > params.thresholdDb)
        {
            gateOpen = true;
            holdCounter = holdSamples;
        }
        else if (gateOpen && keyDb < params.thresholdDb - params.hysteresisDb)
        {
            if (holdCounter > 0)
                --holdCounter;
            else
                gateOpen = false;
        }

        gateTarget = gateOpen ? openGain : closedGain;
        gateGain += (gateTarget > gateGain ? gateAttackCoef : gateReleaseCoef) * (gateTarget - gateGain);

        // ---- per band expansion ----
        float bandGainSum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = channels[ch][i];
            float rest = highCut[ch].process(lowCut[ch].process(dry));
            auto& split = splitters[ch];

            // Bands are summed lowest first; before each new band joins, the
            // running sum goes through that crossover's all-pass so its phase
            // matches what the crossover did to the high side.
            float wet = 0.0f;
            for (int b = 0; b < kBands; ++b)
            {
                float bandSample;
                if (b < kBands - 1)
                {
                    float low, high;
                    split[static_cast<size_t>(b)].process(rest, low, high);
                    bandSample = low;
                    rest = high;
                }
                else
                {
                    bandSample = rest;
                }

                auto& band = bands[ch][static_cast<size_t>(b)];
                const float energy = band.env.process(bandSample);
                // Both channels follow one floor estimate; the left channel
                // owns the bookkeeping so a stereo pair never drifts apart.
                // the louder channel decides (a right-only vocal is still voice): channel 1's envelope is one sample behind
                if (ch == 0)
                    bandTarget[static_cast<size_t>(b)] = computeBandGain(b, band, numChannels > 1 ? std::max(energy, bands[1][static_cast<size_t>(b)].env.value()) : energy);
                const float target = bandTarget[static_cast<size_t>(b)];
                // Open fast, close at the smoothing rate: a consonant must come
                // through whole, the room can leave gracefully.
                const float coef = target > band.gain ? std::max(bandSmoothCoef, 0.25f) : bandSmoothCoef;
                band.gain += coef * (target - band.gain);

                if (b > 0 && b < kBands - 1)
                    wet = split[static_cast<size_t>(b)].allPass(wet);
                // dry / wet / residual are blended per band, inside the same all-pass tree, so a partial Mix never
                // phase-cancels against the untouched input
                const float g = band.gain * gateGain;
                wet += bandSample * (params.listenResidual ? 1.0f - g : dryMix + wetMix * g);
                if (ch == 0)
                    bandGainSum += band.gain;
            }

            (void) dry;
            const float out = wet;

            channels[ch][i] = out * outGain + kTiny - kTiny;
        }

        suppressAccum += bandGainSum / static_cast<float>(kBands);
    }

    if (params.learning)
        learnSamples += numSamples;

    // The minimum tracker only starts once the slow band levels have settled;
    // seeding it from the first, still-rising samples would leave it stuck
    // far below the room.
    if (! minValid)
    {
        samplesSeen += numSamples;
        minValid = samplesSeen >= warmupSamples;
    }

    meterGateDb = gainToDb(gateGain);
    meterSuppressDb = gainToDb(static_cast<float>(suppressAccum / numSamples));
    meterInputDb = energyToDb(inputPeakEnergy);
}

} // namespace hhir
