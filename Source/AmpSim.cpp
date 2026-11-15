// HumHouse Amplitude: guitar/bass amp rig, own DSP (desktop only; phone stays light on CPU).
// Chain: stomp pedals -> preamp (triode stages) -> component-value tone stack (Yeh/Smith TMB) -> power amp (sag, bias,
// presence) -> cabinet + mic (position across the cone, distance, angle) -> room -> post FX -> output. Nonlinear parts run 4x oversampled.
#include "Effects.h"
#include "FastMath.h"
#include <cmath>
#include <complex>

namespace hhs
{
namespace amp
{
struct Model { const char* name; float R1, R2, R3, R4, C1, C2, C3; int stages; float maxGainDb, bright, asym; int cab, mic, power; };
// tone-stack parts follow the classic amp families (250k/1M/25k/56k Fender, 220k/1M/22k/33k Brit ...)
static const Model models[] {
    { "Velvet Deluxe",   250e3f, 1e6f, 10e3f, 100e3f, 250e-12f, 100e-9f, 47e-9f, 2, 32, 0.6f, 0.10f, 0, 0, 0 },
    { "Brit 800",        220e3f, 1e6f, 22e3f,  33e3f, 470e-12f,  22e-9f, 22e-9f, 3, 46, 0.5f, 0.25f, 3, 0, 2 },
    { "Rogue 51",        250e3f, 1e6f, 25e3f,  47e3f, 250e-12f,  22e-9f, 22e-9f, 5, 62, 0.4f, 0.35f, 2, 0, 1 },
    { "Treadplate Dual", 250e3f, 1e6f, 25e3f,  47e3f, 500e-12f,  22e-9f, 22e-9f, 5, 64, 0.3f, 0.30f, 2, 0, 1 },
    { "Citrus 50",       220e3f, 1e6f, 22e3f,  39e3f, 470e-12f,  22e-9f, 22e-9f, 4, 52, 0.3f, 0.30f, 3, 1, 2 },
    { "Crown 30",        1e6f,   1e6f, 10e3f, 100e3f,  50e-12f,  22e-9f, 22e-9f, 2, 38, 0.9f, 0.15f, 1, 4, 3 },
    { "Solo Hundred",    250e3f, 1e6f, 25e3f,  47e3f, 470e-12f,  20e-9f, 20e-9f, 5, 60, 0.5f, 0.30f, 3, 0, 2 },
    { "Blackforest VH",  250e3f, 1e6f, 25e3f,  47e3f, 330e-12f,  22e-9f, 22e-9f, 5, 64, 0.4f, 0.30f, 2, 0, 1 },
    { "Uberhaus",        250e3f, 1e6f, 25e3f,  56e3f, 470e-12f,  22e-9f, 22e-9f, 5, 62, 0.4f, 0.32f, 2, 1, 2 },
    { "Twin Silver",     250e3f, 1e6f, 10e3f, 100e3f, 250e-12f, 100e-9f, 47e-9f, 2, 28, 0.7f, 0.08f, 1, 0, 0 },
    { "Rando 100",       250e3f, 1e6f, 25e3f,  47e3f, 470e-12f,  22e-9f, 22e-9f, 4, 58, 0.3f, 0.20f, 2, 0, 1 },
    { "Powerline",       250e3f, 1e6f, 25e3f,  47e3f, 330e-12f,  22e-9f, 22e-9f, 5, 62, 0.5f, 0.28f, 2, 0, 1 },
    { "Low Ton 810",     250e3f, 1e6f, 25e3f,  56e3f, 250e-12f,  47e-9f, 47e-9f, 2, 34, 0.3f, 0.12f, 4, 5, 1 },
    { "Tweed Empress",   250e3f, 1e6f, 25e3f,  56e3f, 250e-12f,  20e-9f, 20e-9f, 3, 40, 0.5f, 0.15f, 5, 1, 0 },
    { "Five Tre",        250e3f, 1e6f, 25e3f,  47e3f, 470e-12f,  22e-9f, 22e-9f, 5, 64, 0.4f, 0.34f, 2, 0, 1 } };
constexpr int numModels = (int) (sizeof (models) / sizeof (models[0]));

// cabinet: low cut, low resonance, presence peak, cone breakup, high roll-off
struct Cab { const char* name; float hp, resF, resQ, resDb, pkF, pkDb, brkF, brkDb, lp; bool open; };
static const Cab cabs[] {
    { "1x12 Open Alnico",   80, 110, 1.4f, 3, 2200, 3.0f, 3800, 2.0f, 6200, true },
    { "2x12 Open Blue",     75, 105, 1.4f, 3, 2500, 4.0f, 4200, 2.5f, 6800, true },
    { "4x12 Closed V-30",   65,  95, 1.6f, 5, 2600, 5.0f, 4100, 3.0f, 5600, false },
    { "4x12 Closed Green",  65,  90, 1.5f, 5, 2000, 3.5f, 3400, 2.0f, 5200, false },
    { "8x10 Bass",          40,  70, 1.2f, 4, 1800, 2.0f, 3000, 1.0f, 4500, false },
    { "1x12 Tweed Open",    85, 120, 1.2f, 2, 1800, 2.5f, 3200, 1.5f, 5400, true } };
constexpr int numCabs = 6;

struct Mic { const char* name; float lowCut, presF, presDb, hiCut, prox; };
static const Mic mics[] {
    { "HD57 Dynamic",     90, 5000, 5.0f, 12000, 1.0f }, { "HR121 Ribbon",   40, 1500, -1.0f, 8000, 1.3f },
    { "HU87 Condenser",   40, 9000, 2.0f, 18000, 0.6f }, { "HD421 Dynamic",  60, 3500, 3.0f, 13000, 1.1f },
    { "HC414 Condenser",  40, 7000, 3.0f, 17000, 0.7f }, { "HK112 Kick/Bass", 30, 4000, 4.0f, 9000, 1.2f } };
constexpr int numMics = 6;

// pedals: real circuits, our names
enum Pedal { None, GreenScreamer, Rodent, FaceFuzz, BigPuff, GoldenHorse, BlueDrive, OrangeDist, SqueezeComp, CryWah, PhaseNinety, ChoirChorus, AnalogEcho, Tremor, SpringVerb, NoiseGate, numPedals };
static const char* pedalNames[] { "(empty)", "Green Screamer (TS)", "Rodent (RAT)", "Face Fuzz", "Big Puff (Muff)", "Golden Horse (Klon)", "Blue Drive (BD)",
                                   "Orange Dist (DS)", "Squeeze Comp (Dyna)", "Cry Wah", "Phase Ninety", "Choir Chorus (CE)", "Analog Echo (DM)", "Tremor", "Spring Verb", "Gate" };
}

struct OnePole { float a = 0, z = 0; void lp (double sr, double f) { a = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * f / sr)); }
                 float lpT (float x) { z += a * (x - z); return z; } float hpT (float x) { z += a * (x - z); return x - z; } };

// 3rd-order tone stack from component values (Yeh & Smith, DAFx 2006), bilinear transformed
struct ToneStack
{
    double b[4] {}, a[4] {}, z[3] {};
    void set (const amp::Model& m, double fs, float t, float mid, float l)
    {
        l = std::exp ((l - 1.0f) * 3.4f);   // log bass pot
        const double R1 = m.R1, R2 = m.R2, R3 = m.R3, R4 = m.R4, C1 = m.C1, C2 = m.C2, C3 = m.C3, M = mid, T = t, L = l;
        const double b1 = T*C1*R1 + M*C3*R3 + L*(C1*R2 + C2*R2) + (C1*R3 + C2*R3);
        const double b2 = T*(C1*C2*R1*R4 + C1*C3*R1*R4) - M*M*(C1*C3*R3*R3 + C2*C3*R3*R3) + M*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
                        + L*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4) + L*M*(C1*C3*R2*R3 + C2*C3*R2*R3) + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4);
        const double b3 = L*M*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - M*M*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4) + M*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
                        + T*C1*C2*C3*R1*R3*R4 - T*M*C1*C2*C3*R1*R3*R4 + T*L*C1*C2*C3*R1*R2*R4;
        const double a1 = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + M*C3*R3 + L*(C1*R2 + C2*R2);
        const double a2 = M*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3) + L*M*(C1*C3*R2*R3 + C2*C3*R2*R3) - M*M*(C1*C3*R3*R3 + C2*C3*R3*R3)
                        + L*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4) + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4);
        const double a3 = L*M*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - M*M*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4) + M*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4)
                        + L*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4;
        const double c = 2.0 * fs, c2 = c * c, c3 = c2 * c;
        const double B0 = -b1*c - b2*c2 - b3*c3, B1 = -b1*c + b2*c2 + 3*b3*c3, B2 = b1*c + b2*c2 - 3*b3*c3, B3 = b1*c - b2*c2 + b3*c3;
        const double A0 = -1 - a1*c - a2*c2 - a3*c3, A1 = -3 - a1*c + a2*c2 + 3*a3*c3, A2 = -3 + a1*c + a2*c2 - 3*a3*c3, A3 = -1 + a1*c - a2*c2 + a3*c3;
        b[0] = B0 / A0; b[1] = B1 / A0; b[2] = B2 / A0; b[3] = B3 / A0; a[1] = A1 / A0; a[2] = A2 / A0; a[3] = A3 / A0;
    }
    float tick (float x)
    {
        const double y = b[0] * x + z[0];
        z[0] = b[1] * x - a[1] * y + z[1]; z[1] = b[2] * x - a[2] * y + z[2]; z[2] = b[3] * x - a[3] * y;
        if (! std::isfinite (y)) { z[0] = z[1] = z[2] = 0; return 0.0f; }
        return (float) y;
    }
    void clear() { z[0] = z[1] = z[2] = 0; }
};

// 12AX7-ish stage: asymmetric soft clip (grid conduction is harder than cutoff), coupling-cap HPF and Miller LPF around it
static inline float triode (float x, float asym)
{
    const float xp = x + asym * 0.3f;
    const float y = xp >= 0.0f ? hhs::fastTanh (xp) : hhs::fastTanh (xp * (1.0f + asym)) / (1.0f + asym * 0.5f);
    return y - hhs::fastTanh (asym * 0.3f);
}

class AmpRig : public BuiltinEffect
{
public:
    static constexpr int numStomps = 6, numPost = 2;
    AmpRig() { initDefaults(); }
    const char* typeId() const override { return "hhamp"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps = [] {
            std::vector<ParamDef> v;
            std::vector<const char*> mn, cn, mc, pd, rm { "Studio Room A", "Live Room", "Vocal Booth", "Rehearsal Room", "Big Hall" };
            for (auto& m : amp::models) mn.push_back (m.name);
            for (auto& c : amp::cabs) cn.push_back (c.name);
            for (auto& m : amp::mics) mc.push_back (m.name);
            for (auto* p : amp::pedalNames) pd.push_back (p);
            v.push_back ({ "model", "Amp", 0, (float) amp::numModels - 1, 0, mn });
            v.push_back ({ "gain", "Gain", 0, 10, 6.5f }); v.push_back ({ "bass", "Bass", 0, 10, 5.5f }); v.push_back ({ "mid", "Mid", 0, 10, 6 });
            v.push_back ({ "treble", "Treble", 0, 10, 6.5f }); v.push_back ({ "presence", "Presence", 0, 10, 7 }); v.push_back ({ "master", "Master", 0, 10, 7.5f });
            v.push_back ({ "channel", "Channel", 0, 2, 0, { "Clean", "Crunch", "Lead" } });
            v.push_back ({ "power", "Power Amp", 0, 3, 0, { "Class A", "Class AB", "Tube EL34", "Tube EL84" } });
            v.push_back ({ "bias", "Bias", 0, 10, 5 }); v.push_back ({ "sag", "Sag", 0, 10, 5 }); v.push_back ({ "response", "Response", 0, 10, 6.5f });
            v.push_back ({ "cab", "Cabinet", 0, (float) amp::numCabs - 1, 0, cn }); v.push_back ({ "cabmix", "Cab Mix", 0, 100, 100 });
            v.push_back ({ "mic", "Microphone", 0, (float) amp::numMics - 1, 0, mc });
            v.push_back ({ "micx", "Mic X", 0, 1, 0.5f }); v.push_back ({ "micy", "Mic Y", 0, 1, 0.5f });
            v.push_back ({ "micdist", "Distance (ft)", 0.1f, 6, 1 }); v.push_back ({ "micangle", "Angle", 0, 90, 0 });
            v.push_back ({ "room", "Room", 0, 4, 0, rm }); v.push_back ({ "roomsize", "Room Size", 0, 100, 60 }); v.push_back ({ "reflect", "Reflection", 0, 100, 35 });
            v.push_back ({ "damping", "Damping", 0, 100, 70 }); v.push_back ({ "roomwet", "Room Dry/Wet", 0, 100, 20 });
            static const char* eqIds[] { "eq1", "eq2", "eq3", "eq4", "eq5" }; static const char* eqNames[] { "EQ 100", "EQ 400", "EQ 1k", "EQ 3k", "EQ 8k" };
            for (int i = 0; i < 5; ++i) v.push_back ({ eqIds[i], eqNames[i], -12, 12, 0 });
            static const char* sid[numStomps + numPost][5] { { "s1", "s1a", "s1b", "s1c", "s1on" }, { "s2", "s2a", "s2b", "s2c", "s2on" }, { "s3", "s3a", "s3b", "s3c", "s3on" },
                                                            { "s4", "s4a", "s4b", "s4c", "s4on" }, { "s5", "s5a", "s5b", "s5c", "s5on" }, { "s6", "s6a", "s6b", "s6c", "s6on" },
                                                            { "p1", "p1a", "p1b", "p1c", "p1on" }, { "p2", "p2a", "p2b", "p2c", "p2on" } };
            static const char* snm[numStomps + numPost][5] { { "Stomp 1", "S1 Knob 1", "S1 Knob 2", "S1 Knob 3", "S1 On" }, { "Stomp 2", "S2 Knob 1", "S2 Knob 2", "S2 Knob 3", "S2 On" },
                                                            { "Stomp 3", "S3 Knob 1", "S3 Knob 2", "S3 Knob 3", "S3 On" }, { "Stomp 4", "S4 Knob 1", "S4 Knob 2", "S4 Knob 3", "S4 On" },
                                                            { "Stomp 5", "S5 Knob 1", "S5 Knob 2", "S5 Knob 3", "S5 On" }, { "Stomp 6", "S6 Knob 1", "S6 Knob 2", "S6 Knob 3", "S6 On" },
                                                            { "Post FX 1", "P1 Knob 1", "P1 Knob 2", "P1 Knob 3", "P1 On" }, { "Post FX 2", "P2 Knob 1", "P2 Knob 2", "P2 Knob 3", "P2 On" } };
            for (int s = 0; s < numStomps + numPost; ++s)
            {
                v.push_back ({ sid[s][0], snm[s][0], 0, (float) amp::numPedals - 1, 0, pd });
                for (int k = 1; k <= 3; ++k) v.push_back ({ sid[s][k], snm[s][k], 0, 10, 5 });
                v.push_back ({ sid[s][4], snm[s][4], 0, 1, 1, { "Off", "On" } });
            }
            v.push_back ({ "output", "Output (dB)", -24, 12, 0 }); v.push_back ({ "input", "Input (dB)", -12, 24, 0 });
            return v;
        }();
        return ps;
    }
    // param indices
    enum { pModel, pGain, pBass, pMid, pTreble, pPres, pMaster, pChan, pPower, pBias, pSag, pResp, pCab, pCabMix, pMic, pMicX, pMicY, pMicDist, pMicAngle,
           pRoom, pRoomSize, pReflect, pDamp, pRoomWet, pEq1, pSlot0 = pEq1 + 5, pOut = pSlot0 + 5 * (numStomps + numPost), pIn };

    void prepare (double s, int maxBlock) override
    {
        sr = s; block = maxBlock;
        os = std::make_unique<juce::dsp::Oversampling<float>> (1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
        os->initProcessing ((size_t) maxBlock);
        mono.setSize (1, maxBlock); fxBuf.setSize (2, maxBlock);
        verb = createBuiltinEffect ("verb"); verb->prepare (s, maxBlock);
        for (auto& d : dly) { d.assign ((size_t) (s * 1.2) + 4, 0.0f); }
        sprBuf.assign ((size_t) (s * 0.1) + 4, 0.0f);
        reset(); key = -1.0f;
    }
    void reset() override
    {
        ts.clear(); for (auto& f : stageHp) f.z = 0; for (auto& f : stageLp) f.z = 0; sagEnv = 0;
        for (auto& b : cabF) b.clear(); for (auto& b : micF) b.clear(); for (auto& b : eqF) b.clear();
        for (auto& st : stomp) st = {};
        for (auto& st : postR) st = {};
        for (auto& d : dly) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (sprBuf.begin(), sprBuf.end(), 0.0f); if (verb) verb->reset(); if (os) os->reset();
    }
    double tailSeconds() const override { return 3.0; }
    int latencySamples() const override { return os ? (int) std::round (os->getLatencyInSamples()) : 0; }

    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        if (os == nullptr || n > block) return;
        updateCoeffs();
        // guitar in = mono (sum), the rig ends in stereo (room + post FX)
        auto* m = mono.getWritePointer (0);
        const float ing = juce::Decibels::decibelsToGain (p (pIn));
        const float* L = b.getReadPointer (0); const float* R = b.getNumChannels() > 1 ? b.getReadPointer (1) : L;
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]) * ing;
        for (int s = 0; s < numStomps; ++s) runPedal (s, m, n, ctx);
        // amp at 4x
        juce::dsp::AudioBlock<float> blk (mono); auto sub = blk.getSubBlock (0, (size_t) n);
        auto up = os->processSamplesUp (sub);
        runAmp (up.getChannelPointer (0), (int) up.getNumSamples());
        os->processSamplesDown (sub);
        // cab + mic
        const float cm = p (pCabMix) / 100.0f;
        for (int i = 0; i < n; ++i)
        {
            float x = m[i], y = x;
            for (auto& f : cabF) y = f.tick (y, 0);
            for (auto& f : micF) y = f.tick (y, 0);
            const float dl = combTick (y);
            y = (y + combGain * dl) * micLevel;
            y = x + cm * (y - x);
            for (auto& f : eqF) y = f.tick (y, 0);
            if (! std::isfinite (y)) { reset(); y = 0; }
            m[i] = y;
        }
        // stereo: room + post
        auto* oL = b.getWritePointer (0); auto* oR = b.getNumChannels() > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i) { oL[i] = m[i]; if (oR) oR[i] = m[i]; }
        if (p (pRoomWet) > 0.5f && b.getNumChannels() > 1)
        {
            verb->setParamQuiet (0, roomSize); verb->setParamQuiet (1, p (pDamp) / 100.0f); verb->setParamQuiet (3, p (pRoomWet) / 100.0f); verb->setParamQuiet (4, 120.0f);
            verb->process (b, n, ctx);
        }
        for (int s = numStomps; s < numStomps + numPost; ++s) runPost (s, b, n, ctx);
        const float og = juce::Decibels::decibelsToGain (p (pOut));
        b.applyGain (0, n, og);
    }

private:
    double sr = 48000; int block = 512;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    juce::AudioBuffer<float> mono, fxBuf;
    std::unique_ptr<BuiltinEffect> verb;
    ToneStack ts; float tsMakeup = 1;
    OnePole stageHp[6], stageLp[6], presHp, inHp;
    Biquad cabF[5], micF[4], eqF[5];
    float sagEnv = 0, key = -1, micLevel = 1, combGain = 0, roomSize = 0.5f; int combD = 1;
    std::vector<float> comb = std::vector<float> (4096, 0.0f); int combW = 0;
    struct StompState { float z[6] {}; double ph = 0; float env = 0; int w = 0; };
    StompState stomp[numStomps + numPost], postR[numPost];   // postR = right-channel state of the stereo post pedals
    std::vector<float> dly[numStomps + numPost], sprBuf;   // one delay line per pedal slot

    float combTick (float x)
    {
        const int N = (int) comb.size();
        comb[(size_t) combW] = x;
        const float y = comb[(size_t) ((combW - combD + N) % N)];
        combW = (combW + 1) % N; return y;
    }
    const amp::Model& model() const { return amp::models[juce::jlimit (0, amp::numModels - 1, (int) p (pModel))]; }
    void updateCoeffs()
    {
        float k = 0; for (int i = 0; i <= pMicAngle; ++i) k = k * 1.000173f + p (i) * (float) (i + 1);
        k += p (pRoomSize) * 0.37f + (float) sr;
        for (int i = 0; i < 5; ++i) k += p (pEq1 + i) * (float) (i + 17);
        if (k == key) return;
        key = k;
        const auto& md = model(); const double fsO = sr * 4.0;
        ts.set (md, fsO, p (pTreble) / 10.0f, p (pMid) / 10.0f, p (pBass) / 10.0f);
        { ToneStack mid; mid.set (md, sr, 0.5f, 0.5f, 0.5f); tsMakeup = 1.0f / juce::jmax (0.02f, respAt (mid, 700.0)); }
        for (int i = 0; i < 6; ++i) { stageHp[i].lp (fsO, i == 0 ? 30.0 : 80.0 + 40.0 * i); stageLp[i].lp (fsO, 14000.0 - 1500.0 * i); }
        presHp.lp (fsO, 1800.0); inHp.lp (fsO, 25.0);
        const auto& c = amp::cabs[juce::jlimit (0, amp::numCabs - 1, (int) p (pCab))];
        const auto& mc = amp::mics[juce::jlimit (0, amp::numMics - 1, (int) p (pMic))];
        // mic across the speaker: centre (dust cap) = bright and tight, edge of the cone = dark and fat
        const float dx = p (pMicX) - 0.5f, dy = p (pMicY) - 0.5f, rad = juce::jlimit (0.0f, 1.0f, std::sqrt (dx * dx + dy * dy) * 2.0f);
        const float dist = p (pMicDist), ang = p (pMicAngle) / 90.0f;
        const float lp = c.lp * (1.25f - 0.55f * rad) * (1.0f - 0.35f * ang) * (dist < 1 ? 1.0f : 1.0f - 0.04f * (dist - 1));
        const float prox = mc.prox * juce::jlimit (0.0f, 8.0f, 6.0f / (0.6f + dist * 2.0f) - 1.0f) + (c.open ? -1.0f : 1.0f);
        cabF[0].set (Biquad::HighPass, sr, c.hp, 0.7, 0); cabF[1].set (Biquad::Peak, sr, c.resF, c.resQ, c.resDb + prox);
        cabF[2].set (Biquad::Peak, sr, c.pkF * (1.1f - 0.25f * rad), 1.2, c.pkDb * (1.0f - 0.6f * rad)); cabF[3].set (Biquad::Peak, sr, c.brkF, 2.5, c.brkDb - 3.0f * ang);
        cabF[4].set (Biquad::LowPass, sr, juce::jlimit (1500.0f, 16000.0f, lp), 0.75, 0);
        micF[0].set (Biquad::HighPass, sr, mc.lowCut, 0.7, 0); micF[1].set (Biquad::Peak, sr, mc.presF, 1.0, mc.presDb * (1.0f - 0.5f * ang));
        micF[2].set (Biquad::LowPass, sr, mc.hiCut, 0.7, 0); micF[3].set (Biquad::Peak, sr, 300.0, 0.8, rad * 1.5f - 0.5f);
        // floor bounce: a delayed copy (comb) that grows with distance; level falls with distance
        combD = juce::jlimit (1, 4000, (int) (sr * (0.0006 + 0.0011 * dist)));
        combGain = juce::jlimit (0.0f, 0.45f, 0.08f * dist);
        micLevel = 1.0f / (0.7f + 0.3f * dist);
        static const float roomBase[] { 0.45f, 0.7f, 0.15f, 0.35f, 0.92f };
        roomSize = juce::jlimit (0.0f, 1.0f, roomBase[juce::jlimit (0, 4, (int) p (pRoom))] * (0.4f + 1.2f * p (pRoomSize) / 100.0f));
        static const float eqF0[] { 100, 400, 1000, 3000, 8000 };
        for (int i = 0; i < 5; ++i) eqF[i].set (Biquad::Peak, sr, eqF0[i], 0.9, p (pEq1 + i));
    }
    static float respAt (const ToneStack& t, double hz)
    {
        // |H(e^jw)| of the 3rd-order stack (at its own rate)
        const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0;
        std::complex<double> z1 = std::polar (1.0, -w), num = t.b[0], den = 1.0, zz = 1.0;
        for (int i = 1; i < 4; ++i) { zz *= z1; num += t.b[i] * zz; den += t.a[i] * zz; }
        return (float) std::abs (num / den);
    }
    void runAmp (float* x, int n)
    {
        const auto& md = model();
        const int chan = (int) p (pChan);
        const int stages = juce::jlimit (1, 6, md.stages - (chan == 0 ? juce::jmin (2, md.stages - 1) : chan == 1 ? 1 : 0));
        const float gdb = (p (pGain) / 10.0f) * md.maxGainDb * (chan == 0 ? 0.55f : chan == 1 ? 0.8f : 1.0f);
        const float perStage = juce::Decibels::decibelsToGain (gdb / (float) stages) * 1.6f;
        const float master = std::pow (p (pMaster) / 10.0f, 2.0f) * 2.5f + 0.05f;
        const int pw = (int) p (pPower);
        const float bias = p (pBias) / 10.0f, sag = p (pSag) / 10.0f, pres = p (pPres) / 10.0f, resp = p (pResp) / 10.0f;
        const float sagA = 1.0f / (float) (sr * 4.0 * 0.04), sagR = 1.0f / (float) (sr * 4.0 * 0.25);
        const float asymP = pw == 0 ? 0.35f + 0.3f * bias : pw == 3 ? 0.2f : 0.08f + 0.15f * (1.0f - bias);
        for (int i = 0; i < n; ++i)
        {
            float v = inHp.hpT (x[i]);
            v += md.bright * 0.6f * (v - stageLp[5].lpT (v)) * (1.0f - p (pGain) / 12.0f);   // bright cap: more top at low gain
            for (int s = 0; s < stages; ++s)
            {
                v = triode (v * perStage, md.asym);
                v = stageLp[s].lpT (stageHp[s].hpT (v));
            }
            v = ts.tick (v) * tsMakeup;
            // power amp with sag (supply droops under load) and presence (less HF feedback)
            const float lvl = std::abs (v * master);
            sagEnv += (lvl > sagEnv ? sagA : sagR) * (lvl - sagEnv);
            const float drive = master * (1.0f - 0.45f * sag * juce::jmin (1.0f, sagEnv));
            float pv = v + pres * 1.4f * presHp.hpT (v) + (resp - 0.5f) * 0.6f * v;
            pv *= drive;
            float out = hhs::fastTanh (pv + asymP * 0.25f * pv * pv) ;
            if (pw == 1) { const float co = 0.03f * (1.0f - bias); out = out > co ? out - co : out < -co ? out + co : 0.0f; }   // class AB crossover
            x[i] = out * 0.6f / (0.3f + 0.7f * master);
        }
    }
    // ---- pedals (pre-amp, mono)
    void runPedal (int s, float* x, int n, const PlayContext&)
    {
        const int base = pSlot0 + 5 * s, type = (int) p (base);
        if (type == amp::None || p (base + 4) < 0.5f) return;
        const float k1 = p (base + 1) / 10.0f, k2 = p (base + 2) / 10.0f, k3 = p (base + 3) / 10.0f;
        auto& st = stomp[s];
        const float fs = (float) sr;
        auto lpA = [fs] (float f) { return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f / fs); };
        switch (type)
        {
            case amp::GreenScreamer: {   // op-amp gain stage: clean + clipped (720 Hz HPF in the feedback), tone LPF, level
                const float g = 1.0f + 118.0f * k1 * k1, aH = lpA (720.0f), aT = lpA (700.0f + 5000.0f * k2), lv = 0.15f + 1.6f * k3;
                for (int i = 0; i < n; ++i) { st.z[0] += aH * (x[i] - st.z[0]); const float hp = x[i] - st.z[0];
                    const float c = hhs::fastTanh (hp * g * 0.5f) * 0.6f; float y = x[i] + c; st.z[1] += aT * (y - st.z[1]); x[i] = st.z[1] * lv; } break; }
            case amp::Rodent: {          // huge op-amp gain whose bandwidth falls as gain rises, hard diodes to ground, FILTER = LPF
                const float g = 2.0f + 1500.0f * k1 * k1 * k1, aG = lpA (juce::jlimit (3000.0f, 20000.0f, 1.0e6f / g * 3.0f)), aF = lpA (475.0f + 15000.0f * (1.0f - k2) * (1.0f - k2)), lv = 0.1f + 1.4f * k3, aH = lpA (60.0f);
                for (int i = 0; i < n; ++i) { st.z[2] += aH * (x[i] - st.z[2]); float y = (x[i] - st.z[2]) * g; st.z[0] += aG * (y - st.z[0]);
                    y = juce::jlimit (-1.0f, 1.0f, st.z[0] * 0.7f); y = y - y * y * y * 0.15f; st.z[1] += aF * (y - st.z[1]); x[i] = st.z[1] * lv; } break; }
            case amp::FaceFuzz: {        // 2 transistor fuzz: big asymmetric clip, bias = knob 2
                const float g = 3.0f + 80.0f * k1, b = (k2 - 0.5f) * 0.6f, lv = 0.1f + 1.2f * k3, aL = lpA (5500.0f);
                for (int i = 0; i < n; ++i) { float y = x[i] * g + b; y = y > 0 ? 1.0f - std::exp (-y) : -0.7f * (1.0f - std::exp (y * 1.4f)); y -= b * 0.5f;
                    st.z[0] += aL * (y - st.z[0]); x[i] = st.z[0] * lv; } break; }
            case amp::BigPuff: {         // two clipping stages + mid-scooped tone (LP/HP blend)
                const float g = 4.0f + 60.0f * k1, aL = lpA (480.0f), aH = lpA (1000.0f), lv = 0.1f + 1.2f * k3;
                for (int i = 0; i < n; ++i) { float y = hhs::fastTanh (x[i] * g); y = hhs::fastTanh (y * g * 0.4f);
                    st.z[0] += aL * (y - st.z[0]); st.z[1] += aH * (y - st.z[1]); const float t = st.z[0] * (1.0f - k2) + (y - st.z[1]) * k2; x[i] = t * lv * 1.4f; } break; }
            case amp::GoldenHorse: {     // clean + soft germanium clip blend that grows with gain, active treble
                const float g = 1.0f + 40.0f * k1, aT = lpA (1500.0f), lv = 0.2f + 1.5f * k3;
                for (int i = 0; i < n; ++i) { const float c = hhs::fastTanh (x[i] * g) * (0.2f + 0.6f * k1); float y = x[i] * (1.0f - 0.5f * k1) + c;
                    st.z[0] += aT * (y - st.z[0]); y = st.z[0] + (y - st.z[0]) * (0.5f + 1.5f * k2); x[i] = y * lv; } break; }
            case amp::BlueDrive: case amp::OrangeDist: {   // discrete clip (BD: soft, DS: hard), tone
                const bool hard = type == amp::OrangeDist; const float g = 2.0f + (hard ? 250.0f : 60.0f) * k1 * k1, aT = lpA (800.0f + 6000.0f * k2), lv = 0.1f + 1.3f * k3;
                for (int i = 0; i < n; ++i) { float y = x[i] * g; y = hard ? juce::jlimit (-0.8f, 0.8f, y) : hhs::fastTanh (y);
                    st.z[0] += aT * (y - st.z[0]); x[i] = (hard ? st.z[0] + 0.3f * (y - st.z[0]) : st.z[0]) * lv; } break; }
            case amp::SqueezeComp: {     // OTA comp: sensitivity, attack feel, output
                const float th = 0.5f - 0.48f * k1, aA = lpA (200.0f + 800.0f * k2), aR = lpA (4.0f), lv = 0.5f + 2.5f * k3;
                for (int i = 0; i < n; ++i) { const float a = std::abs (x[i]); st.env += (a > st.env ? aA : aR) * (a - st.env);
                    const float gr = st.env > th ? th / st.env : 1.0f; x[i] *= std::pow (gr, 0.85f) * lv; } break; }
            case amp::CryWah: {          // resonant band-pass swept by the pedal (knob 1), Q (knob 2)
                const float f = 400.0f * std::pow (5.5f, k1), q = 2.0f + 8.0f * k2, w = 2.0f * std::sin (juce::MathConstants<float>::pi * f / fs), d = 1.0f / q, lv = 0.5f + 1.5f * k3;
                for (int i = 0; i < n; ++i) { const float hp = x[i] - st.z[0] - d * st.z[1]; st.z[1] += w * hp; st.z[0] += w * st.z[1]; x[i] = (st.z[1] * 2.2f + x[i] * 0.1f) * lv; } break; }
            case amp::NoiseGate: {
                const float th = std::pow (10.0f, (-80.0f + 60.0f * k1) / 20.0f), aR = lpA (2.0f + 30.0f * (1.0f - k2));
                for (int i = 0; i < n; ++i) { const float a = std::abs (x[i]); st.env += (a > st.env ? 0.5f : aR) * (a - st.env); const float tgt = st.env > th ? 1.0f : 0.0f;
                    st.z[0] += (tgt > st.z[0] ? 0.05f : aR) * (tgt - st.z[0]); x[i] *= st.z[0]; } break; }
            default: { float* bufs[] { x, x }; modFx (s, type, bufs, 1, n, k1, k2, k3); break; }
        }
    }
    void runPost (int s, juce::AudioBuffer<float>& b, int n, const PlayContext&)
    {
        const int base = pSlot0 + 5 * s, type = (int) p (base);
        if (type == amp::None || p (base + 4) < 0.5f) return;
        float* bufs[] { b.getWritePointer (0), b.getNumChannels() > 1 ? b.getWritePointer (1) : b.getWritePointer (0) };
        const float k1 = p (base + 1) / 10.0f, k2 = p (base + 2) / 10.0f, k3 = p (base + 3) / 10.0f;
        if (type >= amp::PhaseNinety && type <= amp::SpringVerb) modFx (s, type, bufs, b.getNumChannels() > 1 ? 2 : 1, n, k1, k2, k3);
        else
        {
            runPedal (s, bufs[0], n, {});
            if (b.getNumChannels() > 1) { std::swap (stomp[s], postR[s - numStomps]); runPedal (s, bufs[1], n, {}); std::swap (stomp[s], postR[s - numStomps]); }
        }
    }
    // time / modulation pedals (work in mono pre-amp or stereo post-amp)
    void modFx (int s, int type, float** ch, int nch, int n, float k1, float k2, float k3)
    {
        auto& st = stomp[s]; const double fs = sr; const double twoPi = juce::MathConstants<double>::twoPi;
        auto& d = dly[s];
        if (d.empty()) return;
        const int N = (int) d.size();
        for (int i = 0; i < n; ++i)
        {
            const float in = nch > 1 ? 0.5f * (ch[0][i] + ch[1][i]) : ch[0][i];
            float yL = in, yR = in;
            switch (type)
            {
                case amp::PhaseNinety: {   // 4 all-pass stages swept by an LFO, feedback (knob 2)
                    st.ph += (0.1 + 6.0 * k1 * k1) / fs; const float l = 0.5f + 0.5f * (float) std::sin (twoPi * st.ph);
                    const float a = 1.0f - 2.0f / (1.0f + std::tan ((float) (juce::MathConstants<double>::pi * (200.0 + 1600.0 * l) / fs)) * 1.0f + 1.0f);
                    float v = in + k2 * 0.7f * st.z[5];
                    for (int j = 0; j < 4; ++j) { const float o = -a * v + st.z[j]; st.z[j] = v + a * o; v = o; }
                    st.z[5] = v; yL = yR = 0.5f * (in + v) * (0.6f + 0.8f * k3); break; }
                case amp::ChoirChorus: case amp::Tremor: case amp::AnalogEcho: case amp::SpringVerb: {
                    d[(size_t) st.w] = in;
                    auto rd = [&] (double ago) { double pos = st.w - ago; while (pos < 0) pos += N; const int i0 = (int) pos; const float f = (float) (pos - i0);
                                                 return d[(size_t) i0] + f * (d[(size_t) ((i0 + 1) % N)] - d[(size_t) i0]); };
                    if (type == amp::ChoirChorus)
                    {   // BBD chorus: 5-12 ms delay, triangle LFO, darker wet, L/R opposite phase
                        st.ph += (0.2 + 6.0 * k1) / fs; const double tri = 2.0 * std::abs (2.0 * (st.ph - std::floor (st.ph)) - 1.0) - 1.0;
                        const double base = 0.0075 * fs, dep = (0.0005 + 0.0035 * k2) * fs;
                        float wl = rd (base + dep * tri), wr = rd (base - dep * tri);
                        st.z[0] += 0.35f * (wl - st.z[0]); st.z[1] += 0.35f * (wr - st.z[1]);
                        yL = in + k3 * (st.z[0] - in * 0.0f); yR = in + k3 * st.z[1];
                    }
                    else if (type == amp::Tremor)
                    {
                        st.ph += (1.0 + 11.0 * k1) / fs; const float l = 0.5f + 0.5f * (float) std::sin (twoPi * st.ph);
                        const float g = 1.0f - k2 * (k3 > 0.5f ? (l > 0.5f ? 1.0f : 0.0f) : l); yL = yR = in * g;
                    }
                    else if (type == amp::AnalogEcho)
                    {   // BBD echo: 20-600 ms, dark repeats that saturate
                        const double t = (0.02 + 0.58 * k1) * fs; float w = rd (t);
                        st.z[0] += 0.25f * (w - st.z[0]); d[(size_t) st.w] = in + hhs::fastTanh (st.z[0] * k2 * 1.05f);
                        yL = yR = in + k3 * st.z[0];
                    }
                    else
                    {   // spring: dispersive chirpy all-pass chain into a short drip delay
                        float v = rd (0.033 * fs + 0.004 * fs * std::sin (st.ph += 0.6 / fs));
                        for (int j = 0; j < 4; ++j) { const float a = 0.6f; const float o = -a * v + st.z[j]; st.z[j] = v + a * o; v = o; }
                        st.z[4] += (0.15f + 0.6f * k2) * (v - st.z[4]); d[(size_t) st.w] = in + st.z[4] * (0.3f + 0.6f * k1);
                        yL = in + k3 * st.z[4]; yR = in + k3 * (v * 0.9f);
                    }
                    st.w = (st.w + 1) % N; break; }
                default: break;
            }
            if (nch > 1) { ch[0][i] = yL; ch[1][i] = yR; } else ch[0][i] = 0.5f * (yL + yR);
        }
    }
};

std::unique_ptr<BuiltinEffect> createAmpRig() { return std::make_unique<AmpRig>(); }
}
