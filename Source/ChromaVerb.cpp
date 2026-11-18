// Chroma Verb: dense HumHouse reverb laid out like a classic DAW reverb table (pre-delay, size, diffusion, modulation,
// bass/decay/crossover/damping, dry / early / wet + stereo separation) with a slapback tap for vocals and the master.
// Late field: 4 input all-passes into an 8-line modulated feedback delay network (Householder mix).
#include "Effects.h"
#include <cmath>

namespace hhs
{
namespace
{
class ChromaVerb : public BuiltinEffect
{
public:
    ChromaVerb() { initDefaults(); }
    const char* typeId() const override { return "chromaverb"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "midside", "Mid / Side", 0, 1, 0, { "Stereo", "Mid/Side" } }, { "hcut", "H.Cut (Hz)", 20, 1000, 120 }, { "lcut", "L.Cut (Hz)", 1000, 20000, 12000 },
            { "delay", "Delay (ms)", 0, 250, 20 }, { "tempo", "Tempo Sync", 0, 1, 0, { "Off", "On" } },
            { "size", "Size", 0, 1, 0.6f }, { "mod", "Mod", 0, 1, 0.3f }, { "diff", "Diffusion", 0, 1, 0.8f }, { "speed", "Speed (Hz)", 0.05f, 4, 0.6f },
            { "bass", "Bass x", 0.3f, 2, 1 }, { "dec", "Decay (s)", 0.1f, 20, 2.2f }, { "cross", "Cross (Hz)", 80, 1000, 300 }, { "damp", "Damp (Hz)", 1000, 20000, 7000 },
            { "dry", "Dry", 0, 1, 1 }, { "er", "Early", 0, 1, 0.3f }, { "wet", "Wet", 0, 1, 0.25f }, { "sep", "Sep (width)", 0, 2, 1 },
            { "slap", "Slapback (ms)", 0, 250, 0 }, { "slapmix", "Slap Level", 0, 1, 0 } };
        return ps;
    }
    enum { pMS, pHC, pLC, pDel, pTempo, pSize, pMod, pDiff, pSpeed, pBass, pDec, pCross, pDamp, pDry, pEr, pWet, pSep, pSlap, pSlapMix };
    double tailSeconds() const override { return juce::jmin (25.0, (double) p (pDec) * 1.5 + 0.3); }
    void prepare (double s, int) override
    {
        sr = s;
        pre.assign ((size_t) (s * 0.6) + 8, 0.0f);
        static constexpr int base[8] { 1123, 1291, 1429, 1597, 1777, 1949, 2131, 2311 };
        for (int i = 0; i < 8; ++i) { line[i].assign ((size_t) (base[i] * s / 44100.0 * 2.2) + 64, 0.0f); len[i] = base[i]; }
        static constexpr int ap[4] { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) { apBuf[i].assign ((size_t) (ap[i] * s / 44100.0) + 2, 0.0f); }
        reset();
    }
    void reset() override
    {
        std::fill (pre.begin(), pre.end(), 0.0f); for (auto& l : line) std::fill (l.begin(), l.end(), 0.0f);
        for (auto& a : apBuf) std::fill (a.begin(), a.end(), 0.0f);
        for (auto& v : lpS) v = 0; for (auto& v : loS) v = 0; preW = 0; lineW = 0; for (auto& w : apW) w = 0; ph = 0;
        hp.clear(); lp.clear();
    }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext& ctx) override
    {
        if (pre.empty() || b.getNumChannels() < 2) return;
        auto* L = b.getWritePointer (0); auto* R = b.getWritePointer (1);
        const int P = (int) pre.size();
        double delMs = p (pDel);
        if (p (pTempo) > 0.5f) delMs = juce::jmin (250.0, 60000.0 / juce::jmax (20.0, ctx.bpm) / 8.0 * juce::jmax (1.0, std::round (delMs / 60.0)));   // 1/32-note steps
        const int dS = juce::jlimit (0, P - 2, (int) (delMs * 0.001 * sr)), slS = juce::jlimit (1, P - 2, (int) (p (pSlap) * 0.001 * sr));
        const float size = 0.35f + 0.65f * p (pSize), dec = juce::jmax (0.1f, p (pDec)), bass = p (pBass), diff = 0.45f + 0.3f * p (pDiff);
        const float modDepth = p (pMod) * 12.0f * (float) (sr / 44100.0), phInc = (float) (2.0 * juce::MathConstants<double>::pi * p (pSpeed) / sr);
        const float aD = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * p (pDamp) / (float) sr), aX = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * p (pCross) / (float) sr);
        hp.set (Biquad::HighPass, sr, p (pHC), 0.707, 0); lp.set (Biquad::LowPass, sr, p (pLC), 0.707, 0);
        float g[8], gLo[8]; int d[8];
        for (int i = 0; i < 8; ++i)
        {
            d[i] = juce::jlimit (8, (int) line[i].size() - 32, (int) (len[i] * size * sr / 44100.0 * 2.0));
            const float t = (float) d[i] / (float) sr;
            g[i] = std::pow (10.0f, -3.0f * t / dec); gLo[i] = std::pow (10.0f, -3.0f * t / juce::jmax (0.1f, dec * bass));
        }
        static constexpr float erT[6] { 0.0043f, 0.0097f, 0.0131f, 0.0179f, 0.0233f, 0.0311f }, erG[6] { 0.8f, -0.6f, 0.55f, -0.45f, 0.38f, -0.3f };
        const float dry = p (pDry), er = p (pEr), wet = p (pWet), sep = p (pSep), slm = p (pSlapMix); const bool ms = p (pMS) > 0.5f;
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];
            float x = lp.tick (hp.tick (0.5f * (inL + inR), 0), 0);
            pre[(size_t) preW] = x;
            auto tap = [&] (int back) { return pre[(size_t) ((preW - back + P) % P)]; };
            const float xd = tap (dS);
            float eL = 0, eR = 0;
            for (int k = 0; k < 6; ++k) { const float v = erG[k] * tap (dS + (int) (erT[k] * size * sr)); if (k & 1) eR += v; else eL += v; }
            const float slap = slm > 0 ? tap (slS) : 0.0f;
            preW = (preW + 1) % P;
            float y = xd;
            for (int k = 0; k < 4; ++k)   // input diffusion: dense from the first ms
            {
                auto& a = apBuf[k]; const int N = (int) a.size(); const float z = a[(size_t) apW[k]];
                const float v = y + diff * z; a[(size_t) apW[k]] = v; y = z - diff * v; apW[k] = (apW[k] + 1) % N;
            }
            float o[8], sum = 0;
            ph += phInc; if (ph > 6.2831853f) ph -= 6.2831853f;
            for (int k = 0; k < 8; ++k)
            {
                const auto& ln = line[k]; const int N = (int) ln.size();
                const float md = modDepth * std::sin (ph + (float) k * 0.785f);
                const float rp = (float) (lineW % N) - (float) d[k] - md + (float) N * 2.0f;   // small numbers: float stays sample-exact in long sessions
                const int i0 = (int) rp; const float fr = rp - (float) i0;
                const float a0 = ln[(size_t) (i0 % N)], a1 = ln[(size_t) ((i0 + 1) % N)];
                float v = a0 + fr * (a1 - a0);
                lpS[k] += aD * (v - lpS[k]); v = lpS[k];   // damping
                loS[k] += aX * (v - loS[k]);               // crossover: lows decay by Bass x
                v = loS[k] * gLo[k] + (v - loS[k]) * g[k];
                o[k] = v; sum += v;
            }
            sum *= 0.25f;   // Householder: o - 2/N * sum
            for (int k = 0; k < 8; ++k) { auto& ln = line[k]; ln[(size_t) (lineW % (int) ln.size())] = o[k] - sum + y * 0.35f; }
            ++lineW; if (lineW >= (1 << 30)) lineW = 0;
            float wL = (o[0] + o[2] + o[4] + o[6]) * 0.5f, wR = (o[1] + o[3] + o[5] + o[7]) * 0.5f;
            float m = 0.5f * (wL + wR), sd = 0.5f * (wL - wR) * sep;
            if (ms) { m *= 0.8f; sd *= 1.2f; }
            wL = m + sd; wR = m - sd;
            L[i] = dry * inL + er * eL * 0.5f + wet * wL + slm * slap;
            R[i] = dry * inR + er * eR * 0.5f + wet * wR + slm * slap;
            if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) { reset(); L[i] = inL; R[i] = inR; }
        }
    }
private:
    double sr = 48000;
    std::vector<float> pre, line[8], apBuf[4]; int len[8] {}, preW = 0, lineW = 0, apW[4] {};
    float lpS[8] {}, loS[8] {}, ph = 0; Biquad hp, lp;
};
}
std::unique_ptr<BuiltinEffect> createChromaVerb() { return std::make_unique<ChromaVerb>(); }
}
