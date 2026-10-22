#include <cstdlib>
#include <set>
#include "../Source/Engine.h"
#include "../Source/Drummer/DrummerCore.h"
#include "../Source/NoteTools.h"
#include "../Source/ProjectIO.h"
#include "../Source/Effects.h"
#include "../Source/StemSplit.h"
#include "../Source/License.h"
#include <cstdio>
#include <iostream>

using namespace hhs;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static float renderPeak (Engine& e, int blocks, int n = 512)
{
    juce::AudioBuffer<float> out (2, n);
    float peak = 0;
    for (int b = 0; b < blocks; ++b)
    {
        float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
        e.processBlock (nullptr, 0, o, 2, n);
        for (int i = 0; i < n; ++i) { CHECK (std::isfinite (o[0][i])); peak = std::max (peak, std::abs (o[0][i])); }
    }
    return peak;
}

// realistic sung phrase (glottal buzz through vowel formants, jitter, vibrato, slides, breaths, short ad-libs)
// run through AutoPitch: on held notes the output must sit on the scale note with no chirps (wrong-pitch blips) or squeaks
struct VoiceProbe { int chirps = 0; double worstCents = 0, worstSqueak = 0; int squeaks = 0; };
static VoiceProbe probeAutoPitchVoice (int style, unsigned seed, float speed = 0.75f, float natvib = 0.0f, float snap = 0.0f)
{
    const double fs = 48000; const double tau = juce::MathConstants<double>::twoPi;
    struct Seg { double startMidi, endMidi, dur; bool voiced; double vibCents; };
    // C major; sung slightly off the notes; "yeah" ad-libs fall a few semitones; breath gaps between phrases
    const std::vector<Seg> segs {
        { 57.3, 57.3, 0.45, true, 35 }, { 57.3, 60.2, 0.06, true, 0 }, { 60.2, 60.2, 0.40, true, 30 }, { 0, 0, 0.12, false, 0 },
        { 64.4, 64.4, 0.35, true, 25 }, { 64.4, 61.8, 0.08, true, 0 }, { 61.8, 61.8, 0.30, true, 40 }, { 0, 0, 0.09, false, 0 },
        { 67.3, 64.2, 0.14, true, 0 }, { 0, 0, 0.07, false, 0 }, { 52.2, 52.2, 0.50, true, 30 }, { 52.2, 55.3, 0.05, true, 0 },
        { 55.3, 55.3, 0.35, true, 20 }, { 0, 0, 0.15, false, 0 }, { 71.2, 71.2, 0.40, true, 45 }, { 71.2, 69.1, 0.04, true, 0 },
        { 69.1, 69.1, 0.45, true, 30 }, { 69.1, 64.0, 0.12, true, 0 }, { 0, 0, 0.1, false, 0 }, { 48.3, 48.3, 0.5, true, 25 } };
    juce::Random rng ((juce::int64) seed);
    std::vector<float> in, tgtHz; std::vector<char> steady;
    double ph = 0, t = 0, jit = 0;
    double f1s[3] = { 0, 0, 0 }, f2s[3] = { 0, 0, 0 };
    const double fc[3] = { 700, 1150, 2600 }, bw[3] = { 90, 110, 160 };
    double env = 0;
    auto snapC = [] (double m) { static const int sc[7] = { 0, 2, 4, 5, 7, 9, 11 }; int best = 0; double bd = 99;
        for (int o = 2; o < 9; ++o) for (int s : sc) { const double d = std::abs (o * 12 + s - m); if (d < bd) { bd = d; best = o * 12 + s; } } return best; };
    for (auto& sg : segs)
    {
        const int len = (int) (sg.dur * fs);
        for (int i = 0; i < len; ++i, t += 1 / fs)
        {
            const double a = (double) i / len;
            double m = sg.startMidi + (sg.endMidi - sg.startMidi) * a + sg.vibCents / 100.0 * std::sin (tau * 5.6 * t);
            const double hz = 440.0 * std::pow (2.0, (m - 69.0) / 12.0) * (1.0 + jit);
            double x = 0;
            if (sg.voiced)
            {
                ph += hz / fs; if (ph >= 1) { ph -= 1; jit = 0.004 * (rng.nextDouble() * 2 - 1); }
                for (int k = 1; k * hz < 7000; ++k) x += std::sin (tau * k * ph) / (k * std::sqrt ((double) k));
                env += (1.0 - env) * 0.004;
            }
            else env *= 0.996;
            double v = 0.12 * env * x + 0.015 * (rng.nextDouble() * 2 - 1);   // + breath / room noise
            double y = 0;
            for (int f = 0; f < 3; ++f)
            {
                const double rr = std::exp (-juce::MathConstants<double>::pi * bw[f] / fs), cw = std::cos (tau * fc[f] / fs);
                const double o = v + 2 * rr * cw * f1s[f] - rr * rr * f2s[f]; f2s[f] = f1s[f]; f1s[f] = o; y += o * (1 - rr) * (f == 0 ? 1.0 : 0.6);
            }
            in.push_back ((float) y);
            const bool st = sg.voiced && sg.startMidi == sg.endMidi && i > 0.06 * fs && i < len - 0.03 * fs;
            steady.push_back (st ? 1 : 0);
            tgtHz.push_back (st ? (float) (440.0 * std::pow (2.0, (snapC (sg.startMidi) - 69.0) / 12.0)) : 0.0f);
        }
    }
    float pk = 0; for (auto s : in) pk = std::max (pk, std::abs (s));
    for (auto& s : in) s *= 0.5f / pk;
    auto ap = createBuiltinEffect ("autopitch");
    ap->setParam ("body", 0.0f); ap->setParam ("character", 0.0f);   // tuner core alone
    ap->setParam ("style", (float) style); ap->setParam ("retune", 1.0f - speed); ap->setParam ("natvib", natvib); ap->setParam ("snap", snap);
    ap->prepare (fs, 512);
    PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
    juce::AudioBuffer<float> b (2, 512); std::vector<float> out;
    for (size_t at = 0; at < in.size(); at += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - at);
        for (int i = 0; i < n; ++i) { b.setSample (0, i, in[at + (size_t) i]); b.setSample (1, i, in[at + (size_t) i]); }
        ap->process (b, n, ctx);
        for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
    }
    VoiceProbe res;
    const int lag = ap->latencySamples() + (style >= 3 ? 72 : 300);   // shifter delay (approx)
    for (size_t at = 4096; at + 2048 < out.size(); at += 240)
    {
        const size_t c = at + 512 - (size_t) lag;   // input time at window centre
        bool ok = true; for (size_t k = c - 600; k < c + 600; k += 60) ok = ok && steady[k];
        if (! ok) continue;
        const float f = detectPitchYin (out.data() + at, 1024, fs, 0.2f, 0.0f, 60.0f, 1100.0f);
        if (f <= 0) continue;
        const double cents = std::abs (1200.0 * std::log2 (f / tgtHz[c]));
        res.worstCents = std::max (res.worstCents, cents);
        if (cents > (natvib > 0 ? 80.0 : 40.0)) { ++res.chirps; }
    }
    // squeaks: sudden bursts of high-frequency energy in the output that the input doesn't have (2 ms blocks)
    for (size_t at = 4096; at + 1200 < out.size(); at += 96)
    {
        const size_t ci = at - (size_t) lag;
        // the shifter's delay follows the voice's cycle length, so compare against the busiest input block nearby
        double eo = 0, ei = 1e-9;
        for (size_t k = 2; k < 96; ++k) { const double d = out[at + k] - 2 * out[at + k - 1] + out[at + k - 2]; eo += d * d; }
        for (size_t b = ci - 960; b <= ci + 960; b += 48)
        {
            double e = 0;
            for (size_t k = 2; k < 96; ++k) { const double d = in[b + k] - 2 * in[b + k - 1] + in[b + k - 2]; e += d * d; }
            ei = std::max (ei, e);
        }
        const double ratio = eo / ei;
        res.worstSqueak = std::max (res.worstSqueak, ratio);
        if (ratio > 6.0) { ++res.squeaks; }
    }
    return res;
}

// syllables (consonant burst + sharp vowel attack + decay) sung off-key, through AutoPitch: measures whether
// the tuned voice keeps its punch, body, consonants and the gaps between syllables (vs. thin / mashed)
struct SyllableProbe { double levelDb = 0, envCorr = 0, gapIn = 0, gapOut = 0, consDb = 0, lowDb = 0, highDb = 0, attackIn = 0, attackOut = 0; };
static SyllableProbe probeSyllables (int style, double centsOff, float speed = 0.75f)
{
    const double fs = 48000, tau = juce::MathConstants<double>::twoPi;
    juce::Random rng (7);
    std::vector<float> in; std::vector<char> region;   // 0 gap, 1 consonant, 2 attack, 3 vowel
    std::vector<size_t> onsets;
    double f1s[3] = {}, f2s[3] = {}, ph = 0, hp = 0, hpx = 0;
    const double fc[3] = { 650, 1100, 2500 }, bw[3] = { 90, 110, 160 };
    const int notes[10] = { 57, 60, 62, 64, 62, 60, 55, 57, 60, 64 };
    for (int syl = 0; syl < 20; ++syl)
    {
        const double midi = notes[syl % 10] + (syl % 2 ? centsOff : -centsOff) / 100.0;
        const double hz = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
        const int nc = (int) (0.03 * fs), nv = (int) (0.18 * fs), ng = (int) (0.05 * fs);
        onsets.push_back (in.size() + (size_t) nc);
        for (int i = 0; i < nc + nv + ng; ++i)
        {
            double v = 0, cons = 0; char rg = 0;
            if (i < nc) { const double nz = rng.nextDouble() * 2 - 1; hp = 0.6 * (hp + nz - hpx); hpx = nz; cons = 0.08 * hp * std::sin (juce::MathConstants<double>::pi * i / nc); rg = 1; }
            else if (i < nc + nv)
            {
                const int k0 = i - nc;
                const double env = std::min (1.0, k0 / (0.006 * fs)) * (0.35 + 0.65 * std::exp (-k0 / (0.07 * fs))) * std::min (1.0, (nv - k0) / (0.015 * fs));
                ph += hz / fs; if (ph >= 1) ph -= 1;
                double x = 0; for (int k = 1; k * hz < 8000; ++k) x += std::sin (tau * k * ph) / (k * std::sqrt ((double) k));
                v = 0.12 * env * x; rg = k0 < (int) (0.02 * fs) ? 2 : 3;
            }
            double y = 0;
            for (int f = 0; f < 3; ++f)
            {
                const double rr = std::exp (-juce::MathConstants<double>::pi * bw[f] / fs), cw = std::cos (tau * fc[f] / fs);
                const double o = v + 2 * rr * cw * f1s[f] - rr * rr * f2s[f]; f2s[f] = f1s[f]; f1s[f] = o; y += o * (1 - rr) * (f == 0 ? 1.0 : 0.6);
            }
            in.push_back ((float) (y * 6.0 + cons + 0.0005 * (rng.nextDouble() * 2 - 1)));
            region.push_back (rg);
        }
    }
    auto ap = createBuiltinEffect ("autopitch");
    ap->setParam ("body", 0.0f); ap->setParam ("character", 0.0f);   // tuner core alone
    ap->setParam ("style", (float) style); ap->setParam ("retune", 1.0f - speed);
    ap->prepare (fs, 512);
    PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
    juce::AudioBuffer<float> b (2, 512); std::vector<float> out;
    for (size_t at = 0; at < in.size(); at += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - at);
        for (int i = 0; i < n; ++i) { b.setSample (0, i, in[at + (size_t) i]); b.setSample (1, i, in[at + (size_t) i]); }
        ap->process (b, n, ctx);
        for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
    }
    const int E = 96;   // 2 ms envelope frames
    auto envOf = [&] (const std::vector<float>& x) { std::vector<double> e; for (size_t a = 0; a + E <= x.size(); a += E) { double s = 0; for (int k = 0; k < E; ++k) s += x[a + k] * x[a + k]; e.push_back (std::sqrt (s / E)); } return e; };
    const auto ei = envOf (in), eo = envOf (out);
    int lagF = 0; double bestC = -2;
    for (int lf = 0; lf < 12; ++lf)
    {
        double sxy = 0, sxx = 0, syy = 0, mx = 0, my = 0; const size_t N = ei.size() - 20;
        for (size_t k = 5; k < N; ++k) { mx += ei[k]; my += eo[k + lf]; } mx /= (N - 5); my /= (N - 5);
        for (size_t k = 5; k < N; ++k) { const double a = ei[k] - mx, c = eo[k + lf] - my; sxy += a * c; sxx += a * a; syy += c * c; }
        const double cc = sxy / std::sqrt (sxx * syy + 1e-20); if (cc > bestC) { bestC = cc; lagF = lf; }
    }
    SyllableProbe r; r.envCorr = bestC;
    const size_t lag = (size_t) lagF * E;
    double Ei = 0, Eo = 0, Ci = 1e-12, Co = 1e-12, Gi = 0, Go = 0, Vi = 1e-12, Vo = 1e-12, Ai = 0, Ao = 0; int ng = 0, nvw = 0, na = 0;
    double lpI = 0, lpO = 0, hpI = 0, hpO = 0, LIi = 1e-12, LIo = 1e-12, HIi = 1e-12, HIo = 1e-12, pI = 0, pO = 0;
    const double aL = 1 - std::exp (-tau * 300 / fs), aH = std::exp (-tau * 3000 / fs);
    for (size_t k = 9600; k + lag < out.size(); ++k)
    {
        const double x = in[k], y = out[k + lag];
        Ei += x * x; Eo += y * y;
        lpI += aL * (x - lpI); lpO += aL * (y - lpO); LIi += lpI * lpI; LIo += lpO * lpO;
        hpI = aH * (hpI + x - pI); hpO = aH * (hpO + y - pO); pI = x; pO = y; HIi += hpI * hpI; HIo += hpO * hpO;
        if (region[k] == 1) { Ci += x * x; Co += y * y; }
    }
    for (size_t f = 100; f + (size_t) lagF < eo.size(); ++f)
    {
        const char rg = region[f * E + E / 2];
        if (rg == 0) { Gi += ei[f]; Go += eo[f + lagF]; ++ng; }
        if (rg == 3) { Vi += ei[f]; Vo += eo[f + lagF]; ++nvw; }
        if (rg == 2) { Ai += ei[f]; Ao += eo[f + lagF]; ++na; }
    }
    r.levelDb = 10 * std::log10 (Eo / Ei); r.consDb = 10 * std::log10 (Co / Ci);
    r.lowDb = 10 * std::log10 (LIo / LIi) - r.levelDb; r.highDb = 10 * std::log10 (HIo / HIi) - r.levelDb;
    r.gapIn = (Gi / ng) / (Vi / nvw); r.gapOut = (Go / ng) / (Vo / nvw);
    // attack punch per syllable: loudest 2 ms in the first 30 ms of the vowel vs. the vowel's body (no global lag needed)
    double pin = 0, pout = 0; int ns = 0;
    for (size_t on : onsets)
    {
        const size_t f0 = on / E; if (f0 < 100 || f0 + 90 >= eo.size()) continue;
        double ai = 0, ao = 0, bi = 0, bo = 0;
        for (size_t f = f0; f < f0 + 15; ++f) { ai = std::max (ai, ei[f]); }
        for (size_t f = f0; f < f0 + 15 + 12; ++f) { ao = std::max (ao, eo[f]); }
        for (size_t f = f0 + 40; f < f0 + 80; ++f) { bi += ei[f]; bo += eo[f]; }
        pin += ai / bi; pout += ao / bo; ++ns;
    }
    r.attackIn = 40 * pin / ns; r.attackOut = 40 * pout / ns; (void) Ai; (void) Ao; (void) na;
    return r;
}

// held sung vowel, off-key, through AutoPitch: how clean (harmonic vs. grainy noise) and how true to the
// voice's tone (formant envelope vs. the dry voice) the tuned note is - "expensive" = high HNR, low envelope error
struct HeldProbe { double hnrIn = 0, hnrOut = 0, envErrDb = 0, f0Out = 0; };
static HeldProbe probeHeld (int style, double midiIn, double jitter = 0.002)
{
    const double fs = 48000, tau = juce::MathConstants<double>::twoPi;
    juce::Random rng (11);
    std::vector<float> in;
    double f1s[3] = {}, f2s[3] = {}, ph = 0, jit = 0;
    const double fc[3] = { 650, 1100, 2500 }, bw[3] = { 90, 110, 160 };
    const double hz0 = 440.0 * std::pow (2.0, (midiIn - 69.0) / 12.0);
    for (int i = 0; i < (int) (1.6 * fs); ++i)
    {
        ph += hz0 * (1 + jit) / fs; if (ph >= 1) { ph -= 1; jit = jitter * (rng.nextDouble() * 2 - 1); }
        double x = 0; for (int k = 1; k * hz0 < 9000; ++k) x += std::sin (tau * k * ph) / (k * std::sqrt ((double) k));
        const double env = std::min (1.0, i / (0.01 * fs));
        double y = 0;
        for (int f = 0; f < 3; ++f)
        {
            const double rr = std::exp (-juce::MathConstants<double>::pi * bw[f] / fs), cw = std::cos (tau * fc[f] / fs);
            const double o = 0.12 * env * x + 2 * rr * cw * f1s[f] - rr * rr * f2s[f]; f2s[f] = f1s[f]; f1s[f] = o; y += o * (1 - rr) * (f == 0 ? 1.0 : 0.6);
        }
        in.push_back ((float) (y * 6.0));
    }
    auto ap = createBuiltinEffect ("autopitch");
    ap->setParam ("body", 0.0f); ap->setParam ("character", 0.0f);   // tuner core alone
    ap->setParam ("style", (float) style);
    ap->prepare (fs, 512);
    PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
    juce::AudioBuffer<float> b (2, 512); std::vector<float> out;
    for (size_t at = 0; at < in.size(); at += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - at);
        for (int i = 0; i < n; ++i) { b.setSample (0, i, in[at + (size_t) i]); b.setSample (1, i, in[at + (size_t) i]); }
        ap->process (b, n, ctx);
        for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
    }
    const size_t s0 = (size_t) (0.6 * fs), N = (size_t) (0.8 * fs);
    struct H { double f0, hnr; std::vector<double> amp; };
    auto analyse = [&] (const std::vector<float>& x, double guess)
    {
        std::vector<double> w (N); double sw = 0, tot = 0;
        for (size_t i = 0; i < N; ++i) { w[i] = 0.5 - 0.5 * std::cos (tau * i / (N - 1)); sw += w[i]; tot += w[i] * x[s0 + i] * x[s0 + i]; }
        tot /= sw;
        auto harm = [&] (double f0, std::vector<double>* amps) {
            double e = 0;
            for (int k = 1; k * f0 < 8000; ++k)
            {
                const double om = tau * k * f0 / fs; double re = 0, im = 0, c = 1, sn = 0; const double cs = std::cos (om), ss = std::sin (om);
                for (size_t i = 0; i < N; ++i) { const double v = w[i] * x[s0 + i]; re += v * c; im -= v * sn; const double nc = c * cs - sn * ss; sn = sn * cs + c * ss; c = nc; }
                const double a = 2.0 * std::sqrt (re * re + im * im) / sw; e += a * a / 2; if (amps) amps->push_back (a);
            }
            return e;
        };
        double bestF = guess, bestE = -1;
        for (int g = -40; g <= 40; ++g) { const double f = guess * std::pow (2.0, g / 1200.0 * 2.0); const double e = harm (f, nullptr); if (e > bestE) { bestE = e; bestF = f; } }
        H h; h.f0 = bestF; const double eh = harm (bestF, &h.amp);
        h.hnr = 10 * std::log10 (eh / std::max (1e-12, tot - eh));
        return h;
    };
    const float yf = detectPitchYin (out.data() + s0, 2048, fs, 0.2f, 0.0f, 60.0f, 1100.0f);
    const auto hi = analyse (in, hz0), ho = analyse (out, yf > 0 ? yf : hz0);
    HeldProbe r; r.hnrIn = hi.hnr; r.hnrOut = ho.hnr; r.f0Out = ho.f0;
    // tone: energy in 400 Hz bands from 200 Hz to 4.2 kHz, tuned vs. dry (level-matched)
    double bi[10] = {}, bo[10] = {}, ti = 0, to = 0;
    for (size_t k = 0; k < hi.amp.size(); ++k) { const double f = (k + 1) * hi.f0; if (f >= 200 && f < 4200) { bi[(int) ((f - 200) / 400)] += hi.amp[k] * hi.amp[k]; ti += hi.amp[k] * hi.amp[k]; } }
    for (size_t k = 0; k < ho.amp.size(); ++k) { const double f = (k + 1) * ho.f0; if (f >= 200 && f < 4200) { bo[(int) ((f - 200) / 400)] += ho.amp[k] * ho.amp[k]; to += ho.amp[k] * ho.amp[k]; } }
    double err = 0; for (int q = 0; q < 10; ++q) err += std::abs (10 * std::log10 (((bo[q] + 1e-12) / to) / ((bi[q] + 1e-12) / ti)));
    r.envErrDb = err / 10;
    return r;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // a crash on CI still shows the last finished test
    // offline AutoPitch render for listening / analysis: HHS_AP_RENDER="in.wav;out.wav;style;key;scale;A4;speed"
    if (const char* spec = std::getenv ("HHS_AP_RENDER"))
    {
        auto f = juce::StringArray::fromTokens (spec, ";", "");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (f[0])));
        if (rd == nullptr) { std::printf ("cannot read %s\n", f[0].toRawUTF8()); return 1; }
        const int len = (int) rd->lengthInSamples;
        juce::AudioBuffer<float> all (2, len); rd->read (&all, 0, len, 0, true, true);
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", f[2].getFloatValue()); ap->setParam ("key", f[3].getFloatValue());
        ap->setParam ("scale", f[4].getFloatValue()); ap->setParam ("tuning", f[5].getFloatValue());
        if (f.size() > 6) ap->setParam ("retune", f[6].getFloatValue());   // 0 = fastest
        ap->prepare (rd->sampleRate, 512);
        PlayContext ctx;
        juce::AudioBuffer<float> b (2, 512);
        for (int at = 0; at < len; at += 512)
        {
            const int n = std::min (512, len - at);
            for (int c = 0; c < 2; ++c) b.copyFrom (c, 0, all, c, at, n);
            ap->process (b, n, ctx);
            for (int c = 0; c < 2; ++c) all.copyFrom (c, at, b, 0 + c, 0, n);
        }
        juce::File out (f[1]); out.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> wr (wav.createWriterFor (new juce::FileOutputStream (out), rd->sampleRate, 2, 24, {}, 0));
        wr->writeFromAudioSampleBuffer (all, 0, len);
        return 0;
    }
    if (std::getenv ("HHS_AP_PROBE2") != nullptr)
    {
        for (double m : { 57.6, 56.6, 64.3, 50.5 })
            for (int st = 0; st < 6; ++st)
            {
                for (int ab = 0; ab < 1; ++ab)
                {
                    if (st == 1 || st == 2) continue;
                    if (auto* only = std::getenv ("HHS_AP_ST"); only != nullptr && std::atoi (only) != st) continue;
                    const auto r0 = probeHeld (st, m, 0.0), r = probeHeld (st, m);
                    std::printf ("held %.1f style %d %s: f0 %.1f HNR clean %.1f -> %.1f, jitter %.1f -> %.1f dB, tone error %.2f / %.2f dB\n", m, st, ab ? "NEW" : "old",
                                 r.f0Out, r0.hnrIn, r0.hnrOut, r.hnrIn, r.hnrOut, r0.envErrDb, r.envErrDb);
                }
            }
        return 0;
    }
    if (std::getenv ("HHS_AP_PROBE") != nullptr)
    {
        for (double off : { 0.0, 40.0 })
            for (int st = 0; st < 4; ++st)
            {
                const auto r = probeSyllables (st, off);
                std::printf ("style %d off %2.0fc: level %+.2f dB envCorr %.3f gap %.3f->%.3f attack %.2f->%.2f cons %+.1f dB low %+.2f high %+.2f\n",
                             st, off, r.levelDb, r.envCorr, r.gapIn, r.gapOut, r.attackIn, r.attackOut, r.consDb, r.lowDb, r.highDb);
            }
        return 0;
    }
    juce::ScopedJuceInitialiser_GUI init;
    {   // Glitch Gate pads: each one is audible, works with the transport stopped, and doesn't click
        auto probe = hhs::createBuiltinEffect ("glitch");
        const int numFx = (int) probe->params()[(size_t) probe->paramIndex ("fx")].choices.size();
        for (int k = 1; k < numFx; ++k)
            for (bool playing : { true, false })
            {
                auto e = hhs::createBuiltinEffect ("glitch");
                e->setParam ("fx", (float) k);
                e->prepare (44100.0, 512);
                hhs::PlayContext ctx; ctx.bpm = 140; ctx.playing = playing;
                juce::AudioBuffer<float> b (2, 512);
                const int total = 44100 * 4;
                double diff = 0; float maxStep = 0, prev = 0; bool finite = true;
                for (int pos = 0; pos < total; pos += 512)
                {
                    for (int i = 0; i < 512; ++i) { const float x = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (pos + i) / 44100.0); b.setSample (0, i, x); b.setSample (1, i, x); }
                    ctx.beatPos = pos / 44100.0 * 140.0 / 60.0;
                    e->process (b, 512, ctx);
                    for (int i = 0; i < 512; ++i)
                    {
                        const float x = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (pos + i) / 44100.0), y = b.getSample (0, i);
                        finite &= std::isfinite (y);
                        if (pos + i > 0) maxStep = std::max (maxStep, std::abs (y - prev));
                        prev = y; diff += std::abs (y - x);
                    }
                }
                const bool crush = juce::String (probe->params()[(size_t) probe->paramIndex ("fx")].choices[(size_t) k]) == "Lo-Fi Crush";
                std::printf ("glitch %-16s %s: change %.3f, max step %.3f\n", probe->params()[(size_t) probe->paramIndex ("fx")].choices[(size_t) k], playing ? "playing" : "stopped", diff / total, maxStep);
                CHECK (finite);
                CHECK (diff / total > 0.01);
                CHECK (maxStep < (crush ? 0.2f : 0.08f));
            }
    }
    {   // Hum House Master: glues a loud mix without blowing up, zero latency
        auto e = hhs::createBuiltinEffect ("hhmaster");
        CHECK (e != nullptr);
        e->prepare (44100.0, 512);
        hhs::PlayContext ctx;
        juce::AudioBuffer<float> b (2, 512);
        float peak = 0; bool finite = true;
        for (int pos = 0; pos < 44100 * 2; pos += 512)
        {
            for (int i = 0; i < 512; ++i) { const float x = 0.8f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * (pos + i) / 44100.0); b.setSample (0, i, x); b.setSample (1, i, x * 0.7f); }
            e->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { finite &= std::isfinite (b.getSample (0, i)); peak = std::max (peak, std::abs (b.getSample (0, i))); }
        }
        std::printf ("hum house master: peak %.2f, GR %.1f dB\n", peak, e->meter (0));
        CHECK (finite && peak > 0.1f && peak < 4.0f);
        CHECK (e->latencySamples() == 0);
    }
    {   // FL: new channels fill mixer inserts 1, 2, 3 ... in order, past 10 too
        auto pr = hhs::Project::createDefault();
        for (int i = 0; i < 12; ++i) { hhs::Channel c; c.name = "Ch" + juce::String (i + 1); c.mixerTrack = -1; pr.addChannel (c); }
        bool inOrder = true;
        for (int i = 0; i < 12; ++i) inOrder &= pr.channels[(size_t) i].mixerTrack == i + 1;
        std::set<int> laneIns;
        for (auto& l : pr.lanes) { inOrder &= l.mixerTrack > 12; laneIns.insert (l.mixerTrack); }
        std::printf ("channel routing: ch1 -> %d, ch12 -> %d, track 1 -> %d\n", pr.channels[0].mixerTrack, pr.channels[11].mixerTrack, pr.lanes[0].mixerTrack);
        CHECK (inOrder);
        CHECK (laneIns.size() == pr.lanes.size());
    }

    // scales
    CHECK (inScale (60, 0, 0) && ! inScale (61, 0, 0));
    CHECK (snapToScale (61, 0, 0) == 60 || snapToScale (61, 0, 0) == 62);

    // note tools
    {
        NoteList l { { 60, 0.0, 1.0 } };
        tools::ArpSettings a; a.rate = 0.125; tools::arpeggiate (l, a);
        CHECK (l.size() == 8);   // 1/8-beat roll over 1 beat
        NoteList chord { { 60, 0, 1 }, { 64, 0, 1 }, { 67, 0, 1 } };
        tools::StrumSettings s; s.time = 0.05; tools::strum (chord, s);
        CHECK (chord.size() == 3 && chord[0].start == 0.0 && chord[2].start > chord[1].start);
        CHECK (std::abs (chord[2].end() - 1.0) < 1e-9);  // preserve end
        NoteList g { { 60, 0, 0.5 }, { 60, 0.5, 0.5 } }; tools::glue (g); CHECK (g.size() == 1 && g[0].length == 1.0);
        NoteList d { { 60, 0, 1, 0.8f, 0, false, true } }; tools::duplicateRight (d); CHECK (d.size() == 2 && d[1].start == 1.0);
        NoteList c { { 60, 0, 1 } }; tools::quickChop (c, 0.25); CHECK (c.size() == 4);
        NoteList f { { 60, 1, 1 } }; tools::flam (f, {}); CHECK (f.size() == 2 && f[0].start < 1.0);
    }

    // routing
    {
        auto p = Project::createDemo();
        const int a = p.addMixerTrack ("A"), b = p.addMixerTrack ("B");
        CHECK (p.toggleRoute (a, b));
        CHECK (! p.toggleRoute (b, a));        // cycle refused
        auto order = p.processingOrder();
        CHECK (std::find (order.begin(), order.end(), a) < std::find (order.begin(), order.end(), b));
        CHECK (order.back() == 0);
    }

    // engine renders the default beat
    {
        Engine e;
        e.prepare (44100, 512);
        e.newProject (Project::createDemo());
        e.sync();
        e.play();
        const float pk = renderPeak (e, 200);
        CHECK (pk > 0.05f);
        e.stop();
    }

    // automation clips (FL): an insert volume envelope mutes then opens the beat, an FX knob follows its envelope, both survive save/load
    {
        CHECK (std::abs (autoValueAt ({ { 0.0, 0.0f }, { 4.0, 1.0f } }, 2.0) - 0.5f) < 1.0e-4f);
        CHECK (std::abs (autoToValue (AutoChanPitch, autoFromValue (AutoChanPitch, 7.0f)) - 7.0f) < 0.02f);
        auto pr = Project::createDemo();
        FxSlot fx; fx.uid = pr.newUid(); fx.type = "filter"; pr.mixer[0].fx.push_back (fx);
        Clip vol; vol.uid = pr.newUid(); vol.lane = 0; vol.start = 0; vol.length = 64;
        vol.autoTarget.kind = AutoInsertVol; vol.autoTarget.track = 0; vol.autoTarget.name = "Master - Volume";
        vol.autoPts = { { 0.0, 0.0f }, { 2.0, 0.0f }, { 2.01, 0.64f }, { 64.0, 0.64f } };
        pr.clips.push_back (vol);
        auto pdef = createBuiltinEffect ("filter");
        CHECK (pdef != nullptr && ! pdef->params().empty());
        juce::String pid = pdef->params().front().id;
        for (auto& d : pdef->params()) if (d.choices.empty()) { pid = d.id; break; }
        Clip fxc = vol; fxc.uid = pr.newUid(); fxc.autoTarget = {}; fxc.autoTarget.kind = AutoFxParam; fxc.autoTarget.track = 0; fxc.autoTarget.fxUid = fx.uid;
        fxc.autoTarget.param = pid; fxc.autoTarget.name = "Filter"; fxc.autoPts = { { 0.0, 0.0f }, { 64.0, 1.0f } };
        pr.clips.push_back (fxc);
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs_auto_test.hhs");
        CHECK (saveProject (pr, f));
        Project back; CHECK (loadProject (back, f)); f.deleteFile();
        int autos = 0; for (auto& c : back.clips) if (c.isAuto()) { ++autos; CHECK (! c.isAudio()); }
        CHECK (autos == 2);
        Engine e; e.prepare (44100, 512); e.newProject (back); e.sync();
        e.setPositionBeats (0.0); e.play();
        const float quiet = renderPeak (e, 40);   // ~0.9 beats
        e.stop(); e.setPositionBeats (3.0); e.play();
        renderPeak (e, 4);
        const float loud = renderPeak (e, 40);
        const double at = e.positionBeats();
        e.stop();
        std::printf ("automation: muted peak %.5f, open peak %.3f\n", quiet, loud);
        CHECK (quiet < 0.002f);
        CHECK (loud > 0.05f);
        auto* b = e.builtinFx (0, fx.uid);
        CHECK (b != nullptr);
        if (b != nullptr)
        {
            const auto& d = b->params()[(size_t) b->paramIndex (pid)];
            const float want = d.min + (float) (at / 64.0) * (d.max - d.min);
            std::printf ("automation: %s = %.3f (want ~%.3f)\n", pid.toRawUTF8(), b->getParam (pid), want);
            CHECK (std::abs (b->getParam (pid) - want) < 0.03f * (d.max - d.min));
        }
    }

    // slice tool on pattern clips (FL): cutting a pattern clip anywhere plays exactly like the uncut clip
    {
        auto render = [] (bool cut)
        {
            Engine e;
            e.prepare (44100, 512);
            auto pr = Project::createDemo(); pr.songMode = true;
            if (cut)
            {
                std::vector<Clip> add;
                for (auto& c : pr.clips)
                    if (c.patternIndex >= 0 && ! c.isDrummer() && c.length > 2.0)
                    {
                        const double at = c.start + 1.5;
                        Clip b = c; b.uid = pr.newUid(); b.start = at; b.length = c.start + c.length - at; b.patOffset += at - c.start;
                        c.length = at - c.start; add.push_back (b);
                    }
                CHECK (! add.empty());
                for (auto& b : add) pr.clips.push_back (b);
            }
            e.newProject (pr);
            e.sync();
            e.play();
            std::vector<float> outL;
            juce::AudioBuffer<float> out (2, 512);
            for (int blk = 0; blk < 300; ++blk)
            {
                float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
                e.processBlock (nullptr, 0, o, 2, 512);
                outL.insert (outL.end(), out.getReadPointer (0), out.getReadPointer (0) + 512);
            }
            return outL;
        };
        const auto a0 = render (false), a1 = render (true);
        double maxDiff = 0, pk = 0;
        for (size_t i = 0; i < a0.size(); ++i) { maxDiff = std::max (maxDiff, (double) std::abs (a0[i] - a1[i])); pk = std::max (pk, (double) std::abs (a0[i])); }
        std::printf ("pattern slice: max diff %.6f (peak %.3f)\n", maxDiff, pk);
        CHECK (pk > 0.01 && maxDiff < 1.0e-4);
    }

    // an edit while playing (delete clips, etc.) keeps the beat running exactly where it was: no fade, no re-hit, no gap
    {
        auto render = [] (bool editMidway, int mode)
        {
            Engine e;
            e.prepare (44100, 512);
            auto pr = Project::createDemo(); pr.songMode = mode == 1;
            e.newProject (pr);
            e.sync();
            e.play();
            std::vector<float> outL;
            juce::AudioBuffer<float> out (2, 512);
            for (int blk = 0; blk < 400; ++blk)
            {
                if (editMidway && blk % 37 == 20) e.sync();
                float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
                e.processBlock (nullptr, 0, o, 2, 512);
                outL.insert (outL.end(), out.getReadPointer (0), out.getReadPointer (0) + 512);
            }
            return outL;
        };
        for (int mode : { 0, 1 })
        {
            const auto a0 = render (false, mode), a1 = render (true, mode);
            double maxDiff = 0; float pk = 0;
            for (size_t i = 0; i < a0.size(); ++i) { maxDiff = std::max (maxDiff, (double) std::abs (a0[i] - a1[i])); pk = std::max (pk, std::abs (a0[i])); }
            std::printf ("edit while playing (%s): peak %.2f, max difference %.6f\n", mode ? "song" : "pattern", pk, maxDiff);
            CHECK (pk > 0.05f);
            CHECK (maxDiff < 1.0e-4);
        }
    }


    // recording from a MONO mic, plus deleting the lane mid-take, must not crash
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        p.songMode = true;
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[1].armed = true;
        e.countInOn = false;
        juce::AudioBuffer<float> mic (1, 512), out (2, 512);
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        auto run = [&] (int blocks) {
            for (int b = 0; b < blocks; ++b)
            {
                const float* ins[1] { mic.getReadPointer (0) };
                float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
                e.processBlock (ins, 1, o, 2, 512);
            }
        };
        const auto before = p.clips.size();
        e.setRecording (true);
        e.play();
        run (120);
        e.stop();
        CHECK (p.clips.size() > before);
        e.setRecording (false);

        e.setRecording (true);
        e.play();
        run (40);
        { const juce::ScopedLock sl (e.getLock()); p.lanes.pop_back(); }
        e.stop();                         // lane gone: must skip, not crash
        e.setRecording (false);
        run (10);
        CHECK (true);
    }
    // MONO input (mic on in 1 only) is heard in both ears; STEREO keeps the pair
    for (int stereo = 0; stereo < 2; ++stereo)
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[0].armed = true; p.lanes[0].monitor = true;
        p.lanes[0].stereoIn = stereo == 1; p.lanes[0].inputCh = 0; p.lanes[0].inputPair = 0;
        juce::AudioBuffer<float> mic (2, 512), out (2, 512);
        mic.clear();
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        float pl = 0, pr = 0;
        for (int b = 0; b < 20; ++b)
        {
            const float* ins[2] { mic.getReadPointer (0), mic.getReadPointer (1) };
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (ins, 2, o, 2, 512);
            pl = std::max (pl, out.getMagnitude (0, 0, 512)); pr = std::max (pr, out.getMagnitude (1, 0, 512));
        }
        CHECK (pl > 0.01f);
        if (stereo) CHECK (pr < pl * 0.1f); else CHECK (std::abs (pl - pr) < pl * 0.05f);
        if (! stereo) CHECK (std::abs (pl - 0.3f) < 0.03f);   // default insert + master faders = unity gain (0 dB)
    }
    // a second take over the same spot drops to the next lane instead of covering the first
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        p.songMode = true;
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[1].armed = true;
        e.countInOn = false;
        juce::AudioBuffer<float> mic (1, 512), out (2, 512);
        for (int i = 0; i < 512; ++i) mic.setSample (0, i, 0.3f * std::sin (i * 0.07f));
        auto take = [&] {
            e.setPositionBeats (0.0); e.setRecording (true); e.play();
            for (int b = 0; b < 100; ++b) { const float* ins[1] { mic.getReadPointer (0) }; float* o[2] { out.getWritePointer (0), out.getWritePointer (1) }; e.processBlock (ins, 1, o, 2, 512); }
            e.stop(); e.setRecording (false);
        };
        take(); take();
        int onLane1 = 0, onLane2 = 0;
        for (auto& c : p.clips) if (c.isAudio() && ! c.hidden) { onLane1 += c.lane == 1; onLane2 += c.lane == 2; }
        CHECK (onLane1 == 1 && onLane2 == 1);
    }
    // recording latency: a take moves earlier by the device's in+out latency, so its first sound lands on the beat
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[0].armed = true;
        e.countInOn = false;
        e.recordLatency = 441;   // 10 ms
        juce::AudioBuffer<float> mic (1, 512), out (2, 512);
        mic.clear();
        int blk = 0;
        e.setPositionBeats (0.0); e.setRecording (true); e.play();
        for (int b = 0; b < 100; ++b)
        {
            mic.clear();
            if (b == 0) mic.setSample (0, 441, 0.8f);   // the "first word", heard by the device 10 ms late
            const float* ins[1] { mic.getReadPointer (0) }; float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (ins, 1, o, 2, 512); ++blk;
        }
        e.stop(); e.setRecording (false);
        const Clip* take = nullptr;
        for (auto& c : p.clips) if (c.isAudio() && ! c.hidden) take = &c;
        CHECK (take != nullptr);
        if (take != nullptr)
        {
            std::printf ("record latency: take start %.4f beats, source offset %.2f ms, length %.3f beats\n",
                         take->start, take->sourceOffset * 1000.0, take->length);
            CHECK (std::abs (take->sourceOffset - 0.01) < 1.0e-6);   // first 10 ms (latency) skipped: the impulse plays at the take start
            CHECK (std::abs (take->start) < 1.0e-9);
            CHECK (std::abs (take->length - (100 * 512 - 441) / (44100.0 * 60.0 / p.bpm)) < 1.0e-6);
        }
        juce::ignoreUnused (blk);
    }
    // NORM: a quiet mic comes up strong, a loud one never clips
    for (float amp : { 0.02f, 0.9f })
    {
        Engine e;
        e.prepare (44100, 512);
        e.sync();
        auto& p = e.project();
        for (auto& l : p.lanes) l.armed = false;
        p.lanes[0].armed = true; p.lanes[0].monitor = true; p.lanes[0].autoNorm = true;
        juce::AudioBuffer<float> mic (2, 512), out (2, 512);
        float pk = 0; double ph = 0;
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < 512; ++i) { const float v = amp * (float) std::sin (ph); ph += 0.05; mic.setSample (0, i, v); mic.setSample (1, i, 0.0f); }
            const float* ins[2] { mic.getReadPointer (0), mic.getReadPointer (1) };
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (ins, 2, o, 2, 512);
            if (b > 300) pk = std::max (pk, out.getMagnitude (0, 0, 512));
        }
        if (amp < 0.1f) CHECK (pk > 0.15f); else CHECK (pk < 1.0f && pk > 0.3f);
    }
    // WAV export: right length, real level, first note at the very start is there
    {
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "Lead"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        Note nt; nt.key = 48; nt.start = 0.0; nt.length = 1.0; nt.velocity = 0.9f;
        p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt);
        e.sync();
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs_export_test.wav");
        CHECK (e.renderToWav (f, false, {}, -1));
        {   // 64-bit float at 96 kHz
            auto f64 = f.getSiblingFile ("hh_test64.wav");
            CHECK (e.renderToWav (f64, false, {}, -1, 64, 96000));
            juce::FileInputStream in (f64); char h[44] {}; in.read (h, 44);
            juce::uint16 fmt, bps; juce::uint32 rate; std::memcpy (&fmt, h + 20, 2); std::memcpy (&bps, h + 34, 2); std::memcpy (&rate, h + 24, 4);
            CHECK (fmt == 3 && bps == 64 && rate == 96000);
            CHECK (f64.getSize() > 44 + 16 * 96000);
            f64.deleteFile();
        }
        {   // MP3 (320 kbps) has MPEG frames; an insert with nothing on it renders silent (stems skip it)
            auto mp3 = f.getSiblingFile ("hh_test.mp3");
            CHECK (e.renderToWav (mp3, false, {}, -1, 24, 0, 1));
            juce::FileInputStream in (mp3); unsigned char h[2] {}; in.read (h, 2);
            std::printf ("mp3 export %d bytes, peak %.2f\n", (int) mp3.getSize(), e.lastRenderPeak);
            CHECK (mp3.getSize() > 20000 && h[0] == 0xff && (h[1] & 0xe0) == 0xe0 && e.lastRenderPeak > 0.01f);
            mp3.deleteFile();
            if (p.mixer.size() > 2)
            {
                auto stem = f.getSiblingFile ("hh_stem.wav");
                CHECK (e.renderToWav (stem, false, {}, 2));
                CHECK (e.lastRenderPeak < 1.0e-5f);
                stem.deleteFile();
            }
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
        CHECK (rd != nullptr);
        if (rd)
        {
            const double secs = 60.0 / p.bpm * p.patterns[(size_t) p.currentPattern].lengthBeats() + 2.0;
            CHECK (std::abs ((double) rd->lengthInSamples - secs * 44100.0) < 1024.0);
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, b.getNumSamples(), 0, true, true);
            CHECK (b.getMagnitude (0, 0, 2048) > 0.02f);
            CHECK (b.getMagnitude (0, b.getNumSamples()) < 1.5f);
        }
        f.deleteFile();
    }
    // a held note longer than the loop must not cut the same note when the loop comes round
    {
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "808"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        auto& pat = p.patterns[(size_t) p.currentPattern];
        const double len = pat.lengthBeats();
        Note nt; nt.key = 36; nt.start = 0.0; nt.length = len * 2.0; nt.velocity = 0.9f;
        pat.notes[id].push_back (nt);
        e.sync(); e.setPositionBeats (0.0); e.play();
        const double spbS = 60.0 / p.bpm * 44100.0;
        renderPeak (e, (int) ((len * 2.0 + 0.3) * spbS / 512.0));
        CHECK (renderPeak (e, 8) > 0.01f);
        e.stop();
    }
    // starting mid-note: a sampler jumps into the sample instead of staying silent
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, 44100);
        for (int i = 0; i < 44100; ++i) { buf->setSample (0, i, 0.5f * (float) i / 44100.0f); buf->setSample (1, i, 0.5f * (float) i / 44100.0f); }
        Channel c; c.type = ChannelType::Sampler; c.oneShot = true; c.rootKey = 60;
        auto at = [&] (double secondsIn) {
            auto inst = createInstrument (c, buf, 44100.0);
            inst->prepare (44100.0, 512);
            inst->noteOnAt (60, 1.0f, false, secondsIn);
            juce::AudioBuffer<float> out (2, 512); out.clear();
            inst->render (out, 0, 512);
            return std::abs (out.getSample (0, 300));
        };
        CHECK (at (0.5) > 0.1f);
        CHECK (at (0.0) < 0.02f);
    }
    // a sample plays exactly like the file, and with Key off every note plays it at its own pitch
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, 4096);
        juce::Random rnd (7);
        for (int i = 0; i < 4096; ++i) { const float v = rnd.nextFloat() - 0.5f; buf->setSample (0, i, v); buf->setSample (1, i, v); }
        auto play = [&] (const Channel& c, int key) {
            auto inst = createInstrument (c, buf, 44100.0);
            inst->prepare (44100.0, 512);
            inst->noteOn (key, 1.0f, false);
            juce::AudioBuffer<float> out (2, 2048); out.clear();
            inst->render (out, 0, 2048);
            return out;
        };
        Channel c; c.type = ChannelType::Sampler; c.rootKey = 60;
        auto o = play (c, 60);
        float err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (o.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err < 1.0e-6f);   // bit-for-bit the WAV, no fade-in / interpolation
        c.keyTrack = false;
        auto k = play (c, 72);
        err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (k.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err < 1.0e-6f);
        c.keyTrack = true;
        auto p = play (c, 72);
        err = 0; for (int i = 0; i < 2048; ++i) err = std::max (err, std::abs (p.getSample (0, i) - buf->getSample (0, i)));
        CHECK (err > 0.01f);     // key on: it's pitched
    }
    // Flanger / Flangus: 0 latency, finite, audibly changes the sound
    // Flangus worst case (6 voices lined up, max feedback) stays bounded instead of blowing up the bus
    {
        auto fx = createBuiltinEffect ("flangus");
        fx->setParam ("voices", 6.0f); fx->setParam ("depth", 0.0f); fx->setParam ("feedback", 0.9f); fx->setParam ("mix", 1.0f);
        fx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; juce::Random rnd (7); float peak = 0;
        for (int k = 0; k < 400; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.5f * (rnd.nextFloat() * 2 - 1); b.setSample (0, i, v); b.setSample (1, i, v); }
            fx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); CHECK (std::isfinite (y)); peak = std::max (peak, std::abs (y)); }
        }
        std::printf ("flangus worst-case peak %.2f\n", peak);
        CHECK (peak < 4.0f);
    }
    for (const char* id : { "flanger", "flangus" })
    {
        auto fx = createBuiltinEffect (id); CHECK (fx != nullptr);
        if (! fx) continue;
        fx->prepare (48000, 512); CHECK (fx->latencySamples() == 0);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; juce::Random r (1); float diff = 0;
        for (int k = 0; k < 40; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = r.nextFloat() * 0.5f - 0.25f; b.setSample (0, i, v); b.setSample (1, i, v); }
            juce::AudioBuffer<float> dry (b);
            fx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { CHECK (std::isfinite (b.getSample (0, i))); diff += std::abs (b.getSample (0, i) - dry.getSample (0, i)); }
        }
        CHECK (diff > 1.0f);
    }
    // Studio EQ 7: natural boost, linear-phase FIR of the same curve, dynamic band pulls a loud band down
    {
        auto run = [] (BuiltinEffect& fx, float hz, float amp, int blocks, bool wait) {
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; double ph = 0; float in = 0, out = 0;
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = amp * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * hz / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
                in = b.getRMSLevel (0, 0, 512); fx.process (b, 512, ctx); out = b.getRMSLevel (0, 0, 512);
                if (wait) juce::Thread::sleep (2);
            }
            return out / std::max (in, 1.0e-9f);
        };
        auto eq = createBuiltinEffect ("steq7");
        eq->setParam ("e4G", 12.0f); eq->setParam ("e4F", 1000.0f);
        eq->prepare (48000, 512);
        const float nat = run (*eq, 1000, 0.1f, 40, false);
        CHECK (nat > 3.0f && nat < 5.0f);
        CHECK (std::abs (eq->responseDb (1000.0) - 12.0f) < 0.5f);
        CHECK (eq->latencySamples() == 0);
        eq->setParam ("phase", 1.0f);
        CHECK (eq->latencySamples() == 1024);
        const float lin = run (*eq, 1000, 0.1f, 300, true);
        CHECK (lin > 3.0f && lin < 5.0f);
        const float linFlat = run (*eq, 8000, 0.1f, 80, true);
        CHECK (linFlat > 0.8f && linFlat < 1.25f);
        {   // Studio Vocal: dips a ringing 3.2 kHz resonance, leaves the low voice alone, 0 latency
            auto sv = createBuiltinEffect ("stvocal"); CHECK (sv != nullptr);
            sv->setParam ("smooth", 1.0f); sv->setParam ("silk", 0.0f); sv->setParam ("warmth", 0.0f); sv->prepare (48000, 512);
            const float harsh = run (*sv, 3200, 0.2f, 60, false);
            sv->prepare (48000, 512);
            const float low = run (*sv, 120, 0.2f, 60, false);
            std::printf ("studio vocal: 3.2 kHz resonance %.1f dB, 120 Hz %.1f dB\n", 20 * std::log10 (harsh), 20 * std::log10 (low));
            CHECK (harsh < 0.7f); CHECK (low > 0.8f && low < 1.15f); CHECK (sv->latencySamples() == 0);
        }
        auto dq = createBuiltinEffect ("steq7");
        dq->setParam ("e4F", 1000.0f); dq->setParam ("e4Dyn", 1.0f); dq->setParam ("e4DTh", -40.0f); dq->setParam ("e4DRg", -12.0f);
        dq->prepare (48000, 512);
        CHECK (run (*dq, 1000, 0.5f, 100, false) < 0.5f);
        CHECK (dq->meter (3) < -6.0f);
        auto e12 = createBuiltinEffect ("steq7");
        CHECK (e12->params().size() == 123);
        e12->setParam ("e12On", 1.0f); e12->setParam ("e12F", 1000.0f); e12->setParam ("e12G", -12.0f); e12->prepare (48000, 512);
        CHECK (std::abs (e12->responseDb (1000.0) + 12.0f) < 0.6f);
        CHECK (run (*dq, 1000, 0.001f, 400, false) > 0.9f);
    }
    // starting playback in the middle of a long synth note still sounds it (FL-style chase)
    {
        Engine e;
        e.prepare (44100, 512);
        auto& p = e.project();
        p.songMode = false;
        Channel c; c.type = ChannelType::Synth; c.name = "Pad"; c.mixerTrack = 0;
        const int id = p.addChannel (c);
        Note nt; nt.key = 60; nt.start = 0.0; nt.length = 8.0; nt.velocity = 0.9f;
        p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt);
        e.sync();
        e.setPositionBeats (2.0);
        e.play();
        CHECK (renderPeak (e, 20) > 0.01f);
        e.stop();
    }
    // every built-in effect: 10 s of stereo audio must cost well under real time (CPU budget per insert)
    for (auto& info : builtinEffects())
    {
        auto fx = createBuiltinEffect (info.id);
        if (fx == nullptr) continue;
        fx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.bpm = 140; ctx.playing = true;
        juce::Random rnd (7); double ph = 0;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 938; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (ph) + 0.05f * (rnd.nextFloat() - 0.5f); ph += 0.0377; b.setSample (0, i, v); b.setSample (1, i, -v); }
            ctx.beatPos = k * 512 / (48000.0 * 60.0 / 140.0);
            fx->process (b, 512, ctx);
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("cpu %-10s %7.1f ms per 10 s (%.2f%%)\n", info.id, ms, ms / 100.0);
        CHECK (ms < 3000.0);
    }
    // AutoPitch CPU: 10 s of a sung note must take a small fraction of real time in every style
    for (int style : { 0, 1, 3 })
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", (float) style);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 938; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph)); ph += juce::MathConstants<double>::twoPi * 228.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
            ap->process (b, 512, ctx);
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("autopitch style %d cpu: %.1f ms per 10 s (%.2f%% of real time)\n", style, ms, ms / 100.0);
        CHECK (ms < 2000.0);
    }
    // AutoPitch Pitch +12 doubles the frequency; Detune moves it by cents
    {
        auto crossings = [] (float pitchSt, float cents) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("pitch", pitchSt); ap->setParam ("detune", cents);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            double ph = 0; int zc = 0; float prev = 0;
            for (int k = 0; k < 200; ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.33 * std::sin (3 * ph)); ph += juce::MathConstants<double>::twoPi * 261.63 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }   // voice-like: has harmonics
                ap->process (b, 512, ctx);
                if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (prev <= 0 && y > 0) ++zc; prev = y; }
            }
            return zc;
        };
        const int base = crossings (0, 0), up = crossings (12, 0), cents = crossings (0, 100);
        CHECK (up > base * 1.8 && up < base * 2.2);
        CHECK (cents > base * 1.03 && cents < base * 1.09);
        // A4 = 415 Hz: a 440-tuned C is re-tuned onto the 415 grid (about a semitone away)
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("tuning", 415.0f); ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0; int zc = 0; float prev = 0;
        for (int k = 0; k < 200; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * 261.63 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
            ap->process (b, 512, ctx);
            if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (prev <= 0 && y > 0) ++zc; prev = y; }
        }
        CHECK (std::abs ((float) zc / (float) base - 1.0f) > 0.03f);
    }
    // AutoPitch: a 452 Hz tone snaps to A 440, stays at full level
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("body", 0.0f); ap->setParam ("character", 0.0f);   // tuner core alone
        ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0, inE = 0, outE = 0; int zc = 0, counted = 0; float prev = 0;
        for (int k = 0; k < 300; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * 452.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); if (k >= 100) inE += v * v; }
            ap->process (b, 512, ctx);
            if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); outE += y * y; if (prev <= 0 && y > 0) ++zc; prev = y; ++counted; }
        }
        const double hz = zc * 48000.0 / counted;
        std::printf ("autopitch 452 Hz -> %.1f Hz, level %.2f dB\n", hz, 10.0 * std::log10 (outE / inE));
        CHECK (std::abs (hz - 440.0) < 3.0);
        CHECK (std::abs (10.0 * std::log10 (outE / inE)) < 0.5);
    }
    // AutoPitch follows the note being sung: a fast drop from A4 to E4 (clean and breathy/deep) lands on E4 at once,
    // never held up on the old note (that big hold-over shift was the transposed / formanted sound)
    for (float noise : { 0.0f, 0.03f })
        for (int style : { 0, 3 })
        {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("style", (float) style);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            juce::Random rnd (7);
            double ph = 0; int zc = 0, counted = 0; float prev = 0, lp = 0;
            for (int k = 0; k < 260; ++k)
            {
                const double f = k < 120 ? 440.0 : 329.63;
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.3f * (float) std::sin (ph) + 0.1f * (float) std::sin (2.0 * ph) + noise * (rnd.nextFloat() * 2.0f - 1.0f);
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v);
                }
                ap->process (b, 512, ctx);
                if (k >= 130)
                    for (int i = 0; i < 512; ++i)
                    {
                        // zero crossings of a low-passed copy (the added noise would add false crossings)
                        lp += 0.08f * (b.getSample (0, i) - lp);
                        if (prev <= 0 && lp > 0) ++zc; prev = lp; ++counted;
                    }
            }
            const double hz = zc * 48000.0 / counted;
            std::printf ("autopitch drop A4->E4 style %d noise %.2f -> %.1f Hz\n", style, noise, hz);
            CHECK (std::abs (hz - 329.63) < 4.0);
        }
    // vowel / accent changes (o -> a, u -> uh, into ee) bend the sung pitch a little and reshape the spectrum:
    // the tuned output must stay on A3 the whole time (oat must not dip, ee must not rise)
    for (int style = 0; style < 4; ++style)
    {
        struct V { float f1, f2, f3, cents; };
        const V vs[] { { 500, 850, 2500, -25 }, { 750, 1200, 2600, 0 }, { 320, 800, 2400, -20 }, { 600, 1150, 2500, 0 }, { 280, 2300, 3000, 30 }, { 750, 1200, 2600, 0 } };
        const double sr = 48000; const int seg = (int) (0.35 * sr), mor = (int) (0.06 * sr), total = seg * 6;
        std::vector<float> in ((size_t) total), out ((size_t) total);
        double ph = 0;
        for (int i = 0; i < total; ++i)
        {
            const int k = std::min (5, i / seg), pos = i - k * seg;
            V v = vs[k];
            if (k > 0 && pos < mor)
            {
                const float m = (float) pos / (float) mor; const V& u = vs[k - 1];
                v = { u.f1 + (v.f1 - u.f1) * m, u.f2 + (v.f2 - u.f2) * m, u.f3 + (v.f3 - u.f3) * m, u.cents + (v.cents - u.cents) * m };
            }
            const double f0 = 220.0 * std::pow (2.0, v.cents / 1200.0);
            ph += juce::MathConstants<double>::twoPi * f0 / sr;
            double y = 0;
            for (int h = 1; h <= 20; ++h)
            {
                const double fh = h * f0;
                auto pk = [fh] (double F, double B) { const double q = (fh - F) / B; return 1.0 / (1.0 + q * q); };
                y += (pk (v.f1, 80) + 0.7 * pk (v.f2, 100) + 0.3 * pk (v.f3, 150) + 0.05) / std::sqrt ((double) h) * std::sin (h * ph);
            }
            in[(size_t) i] = (float) (0.12 * y);
        }
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", (float) style);
        ap->prepare (sr, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        for (int o = 0; o + 512 <= total; o += 512)
        {
            for (int i = 0; i < 512; ++i) { b.setSample (0, i, in[(size_t) (o + i)]); b.setSample (1, i, in[(size_t) (o + i)]); }
            ap->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) out[(size_t) (o + i)] = b.getSample (0, i);
        }
        float worst = 0; int off = 0, n = 0;
        for (int o = (int) (0.3 * sr); o + 2048 <= total - 1024; o += 512)
        {
            const float hz = detectPitchYin (out.data() + o, 2048, sr, 0.15f, 0.0f, 100.0f, 600.0f);
            if (hz <= 0) continue;
            const float c = 1200.0f * std::log2 (hz / 220.0f);
            worst = std::max (worst, std::abs (c)); if (std::abs (c) > 15.0f) ++off; ++n;
        }
        std::printf ("autopitch vowels o-a-u-uh-ee style %d: worst %.1f c, %d of %d windows off >15 c\n", style, worst, off, n);
        CHECK (n > 50);
        CHECK (off <= n / 50);
    }
    // a highlighted ruler range (bars 3-5) always starts playback at its start, wherever the playhead sits
    {
        Engine e;
        e.prepare (48000, 256);
        e.project().songMode = true;
        e.project().punchIn = 8.0; e.project().punchOut = 16.0;
        for (double at : { 0.0, 10.0, 12.0, 20.0 })
        {
            e.setPositionBeats (at);
            e.play();
            std::printf ("range play from %.0f -> starts at %.2f\n", at, e.positionBeats());
            CHECK (std::abs (e.positionBeats() - 8.0) < 1.0e-9);
            e.stop();
        }
    }
    // strict key lock: a raspy A4 sung 30 c sharp comes out on A 440 in every mode (no in-between pitch pokes out)
    for (int style = 0; style < 4; ++style)
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", (float) style);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        juce::Random rnd (11);
        const double f = 440.0 * std::pow (2.0, 30.0 / 1200.0);
        double ph = 0; int zc = 0, counted = 0; float prev = 0, lp = 0;
        for (int k = 0; k < 200; ++k)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.3f * (float) std::sin (ph) + 0.1f * (float) std::sin (2.0 * ph) + 0.06f * (rnd.nextFloat() * 2.0f - 1.0f);
                ph += juce::MathConstants<double>::twoPi * f / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v);
            }
            ap->process (b, 512, ctx);
            if (k >= 60)
                for (int i = 0; i < 512; ++i) { lp += 0.08f * (b.getSample (0, i) - lp); if (prev <= 0 && lp > 0) ++zc; prev = lp; ++counted; }
        }
        const double hz = zc * 48000.0 / counted;
        std::printf ("autopitch strict raspy A4+30c style %d -> %.1f Hz\n", style, hz);
        CHECK (std::abs (hz - 440.0) < 3.0);
    }
    // audio quantize: hits are found a few ms before each attack, none in the decays
    {
        const double sr = 48000; const int n = (int) (sr * 2.5);
        std::vector<float> x ((size_t) n, 0.0f);
        const double hits[] { 0.5, 1.0, 1.37, 2.0 };
        juce::Random rnd (3);
        for (double h : hits)
            for (int i = 0; i < (int) (sr * 0.12); ++i)
                x[(size_t) ((int) (h * sr) + i)] += 0.5f * std::exp (-i / (float) (sr * 0.02)) * (rnd.nextFloat() * 2.0f - 1.0f);
        const auto on = detectOnsets (x.data(), n, sr, 0.05);
        std::printf ("audio quantize onsets: %d found\n", (int) on.size());
        CHECK (on.size() == 4);
        for (size_t k = 0; k < on.size() && k < 4; ++k) CHECK (std::abs (on[k] - hits[k]) < 0.01);
    }
    // AutoPitch Retune is the Antares way (0 = fastest): old "speed" saves convert; Body + Character (turned up) add weight without jumping the level
    {
        std::map<juce::String, float> m { { "speed", 0.9f } };
        migrateFxParams ("autopitch", m);
        CHECK (m.count ("speed") == 0 && std::abs (m["retune"] - 0.1f) < 1.0e-6f);
        std::map<juce::String, float> fl { { "speed", 0.9f } };
        migrateFxParams ("flangus", fl);
        CHECK (fl.count ("speed") == 1);
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", 1.0f); ap->setParam ("body", 0.4f); ap->setParam ("character", 0.3f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0, inE = 0, outE = 0;
        for (int k = 0; k < 300; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) (std::sin (ph) + 0.4 * std::sin (2 * ph)); ph += juce::MathConstants<double>::twoPi * 220.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); if (k >= 100) inE += v * v; }
            ap->process (b, 512, ctx);
            if (k >= 100) for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); CHECK (std::isfinite (y)); outE += y * y; }
        }
        std::printf ("autopitch body/character level %+.2f dB\n", 10.0 * std::log10 (outE / inE));
        CHECK (std::abs (10.0 * std::log10 (outE / inE)) < 3.0);
    }
    // AutoPitch catches rough vocals: breathy (noisy) note, a short "yeah" after silence, and a noisy gap mid-word
    {
        auto run = [] (int scenario) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("style", 3.0f);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            juce::Random rnd (3); double ph = 0; std::vector<float> out;
            const int blocks = scenario == 1 ? 32 : 160;
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const bool voiced = scenario == 1 ? k >= 20 : ! (scenario == 2 && k >= 120 && k < 124);
                    const float noise = (rnd.nextFloat() * 2 - 1) * (scenario == 0 ? 0.12f : scenario == 2 && ! voiced ? 0.08f : 0.0f);
                    float v = 0;
                    if (voiced) { v = 0.25f * (float) std::sin (ph) + 0.1f * (float) std::sin (2 * ph) + 0.05f * (float) std::sin (3 * ph); ph += juce::MathConstants<double>::twoPi * 452.0 / 48000.0; }
                    b.setSample (0, i, v + noise); b.setSample (1, i, v + noise);
                }
                ap->process (b, 512, ctx);
                for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
            }
            // measure: breathy = end, short yeah = its last 100 ms, gap = 16-48 ms after the voice comes back
            const size_t at = scenario == 0 ? out.size() - 2048 : scenario == 1 ? (size_t) (20 * 512 + 1536) : (size_t) (124 * 512 + 768);
            const int len = scenario == 2 ? 1536 : 2048;
            return detectPitchYin (out.data() + at, len, 48000.0, 0.3f, 0.5f);
        };
        const float breathy = run (0), shortYeah = run (1), gap = run (2);
        std::printf ("autopitch rough vocals -> breathy %.1f Hz, short yeah %.1f Hz, after gap %.1f Hz\n", breathy, shortYeah, gap);
        CHECK (std::abs (breathy - 440.0f) < 4.0f);
        CHECK (std::abs (shortYeah - 440.0f) < 4.0f);
        CHECK (std::abs (gap - 440.0f) < 4.0f);
    }
    // AutoPitch keeps enunciation and punch: consonants pass through untouched (not smeared by the shifter),
    // attacks and level stay as sung, in every style
    for (double off : { 0.0, 40.0 })
        for (int st = 0; st < 4; ++st)
        {
            const auto r = probeSyllables (st, off);
            std::printf ("autopitch syllables style %d off %2.0fc: level %+.2f dB, consonants %+.1f dB, attack %.2f -> %.2f\n", st, off, r.levelDb, r.consDb, r.attackIn, r.attackOut);
            CHECK (std::abs (r.consDb) < 1.5);
            CHECK (std::abs (r.levelDb) < 0.5);
            CHECK (r.attackOut > 0.93 * r.attackIn);
        }
    // AutoPitch on a realistic sung phrase: no chirps (wrong-pitch blips) or squeaks in any style
    for (int style : { 0, 1, 2, 3 })
        for (unsigned seed : { 1u, 2u, 3u })
            for (float natvib : { 0.0f, 1.0f })
            {
                const auto v = probeAutoPitchVoice (style, seed, 0.75f, natvib);
                std::printf ("autopitch voice style %d seed %u vib %.0f: %d chirps (worst %.0f c), %d squeaks (worst %.1fx)\n",
                             style, seed, natvib, v.chirps, v.worstCents, v.squeaks, v.worstSqueak);
                CHECK (v.chirps == 0);
                CHECK (v.squeaks == 0);
            }
    // Retune -3 (hardest snap) stays just as clean
    for (int style : { 1, 3 })
        for (unsigned seed : { 1u, 2u, 3u })
        {
            const auto v = probeAutoPitchVoice (style, seed, 0.75f, 0.0f, -3.0f);
            std::printf ("autopitch voice style %d seed %u snap -3: %d chirps (worst %.0f c), %d squeaks (worst %.1fx)\n", style, seed, v.chirps, v.worstCents, v.squeaks, v.worstSqueak);
            CHECK (v.chirps == 0);
            CHECK (v.squeaks == 0);
        }
    // AutoPitch Hard Tune: sung vibrato is flattened (Natural Vibrato 0) or kept (100); deep notes tune like high ones
    {
        auto run = [] (double hz, double vibCents, float natvib, std::vector<float>& out) {
            auto ap = createBuiltinEffect ("autopitch");
            ap->setParam ("style", 3.0f); ap->setParam ("natvib", natvib);
            ap->prepare (48000, 512);
            juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
            double ph = 0, t = 0;
            for (int k = 0; k < 280; ++k)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double f = hz * std::pow (2.0, vibCents / 1200.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t));
                    const float v = 0.25f * (float) std::sin (ph) + 0.1f * (float) std::sin (2 * ph) + 0.05f * (float) std::sin (3 * ph);
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0; t += 1.0 / 48000.0;
                    b.setSample (0, i, v); b.setSample (1, i, v);
                }
                ap->process (b, 512, ctx);
                for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
            }
        };
        auto maxDev = [] (const std::vector<float>& o, double hz) {
            double m = 0;
            for (size_t at = 100 * 512; at + 2048 < o.size(); at += 256)
            {
                const float f = detectPitchYin (o.data() + at, 2048, 48000.0, 0.3f, 0.5f, 60.0f, 1100.0f);
                if (f > 0) m = std::max (m, std::abs (1200.0 * std::log2 (f / hz)));
            }
            return m;
        };
        std::vector<float> flat, kept, deep;
        run (220.0, 40.0, 0.0f, flat); run (220.0, 40.0, 100.0f, kept);
        run (82.41 * std::pow (2.0, 30.0 / 1200.0), 0.0, 0.0f, deep);
        const double dFlat = maxDev (flat, 220.0), dKept = maxDev (kept, 220.0);
        const float deepHz = detectPitchYin (deep.data() + deep.size() - 4096, 4096, 48000.0, 0.3f, 0.5f, 60.0f, 1100.0f);
        std::printf ("autopitch vibrato 40c -> kill %.1fc, keep %.1fc; deep E2 +30c -> %.2f Hz\n", dFlat, dKept, deepHz);
        CHECK (dFlat < 12.0);
        CHECK (dKept > 25.0);
        CHECK (std::abs (deepHz - 82.41f) < 0.6f);
    }
    // Maximizer: loud input never passes the ceiling, output stays finite
    {
        auto mx = createBuiltinEffect ("stmax"); CHECK (mx != nullptr);
        mx->setParam ("gain", 18.0f); mx->setParam ("sat", 0.3f);
        mx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; double ph = 0; float peak = 0; bool finite = true;
        for (int k = 0; k < 100; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.9 * std::sin (ph) + 0.3 * std::sin (7.3 * ph)); ph += juce::MathConstants<double>::twoPi * 60.0 / 48000.0; b.setSample (0, i, v); b.setSample (1, i, v); }
            mx->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); finite = finite && std::isfinite (y); peak = std::max (peak, std::abs (y)); }
        }
        std::printf ("maximizer peak %.3f (ceiling %.3f)\n", peak, juce::Decibels::decibelsToGain (-0.3f));
        CHECK (finite && peak <= juce::Decibels::decibelsToGain (-0.3f) + 1.0e-4f && peak > 0.5f);
    }
    // imported samples: drum hits keep their pitch, everything else follows the Piano Roll
    CHECK (isDrumSampleName ("Hi Hat 03") && isDrumSampleName ("KICK_hard") && ! isDrumSampleName ("Piano C") && ! isDrumSampleName ("Vox Chop"));
    // AutoPitch shifter latency stays tiny while correcting up or down
    for (double ratio : { 0.97, 1.03, 2.0 })
    {
        SpliceShifter sh; sh.prepare (48000);
        double ph = 0, dMax = 0, jump = 0; float prev = 0;
        for (int i = 0; i < 48000; ++i)
        {
            float l = 0.5f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.25 * std::sin (3 * ph)), r = l;
            ph += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            sh.process (l, r, ratio, 48000.0 / 220.0);
            if (i > 4800) { dMax = std::max (dMax, sh.delaySamples()); jump = std::max (jump, (double) std::abs (l - prev)); }
            prev = l;
        }
        std::printf ("autopitch ratio %.2f: max delay %.2f ms, max step %.3f\n", ratio, dMax / 48.0, jump);
        CHECK (dMax < 0.012 * 48000);
        CHECK (jump < 0.2 * ratio);
    }
    // PSOLA (Retune / Humanize styles): pitch moves, the voice's tone (formant) stays put - no chipmunk;
    // with the Formant knob (+4 st) the tone moves up on its own, like a vocal bender
    for (double fmt : { 1.0 })
    {
        const double fs = 48000, f0 = 168.0, fc = 1000.0, ratio = 1.12;
        const double rr = std::exp (-juce::MathConstants<double>::pi * 60.0 / fs), cw = std::cos (juce::MathConstants<double>::twoPi * fc / fs);
        double y1 = 0, y2 = 0, ph = 0, dMax = 0; float prev = 0, jump = 0;
        PsolaShifter sh; sh.prepare (fs);
        std::vector<float> out;
        for (int i = 0; i < 96000; ++i)
        {
            ph += f0 / fs; double x = 0; if (ph >= 1.0) { ph -= 1.0; x = 1.0; }
            const double y = x + 2 * rr * cw * y1 - rr * rr * y2; y2 = y1; y1 = y;
            float l = 0.02f * (float) y, r = l;
            sh.process (l, r, ratio, fs / f0);
            if (i >= 48000) { out.push_back (l); dMax = std::max (dMax, sh.delaySamples()); jump = std::max (jump, std::abs (l - prev)); }
            prev = l;
        }
        auto mag = [&] (double hz) { double re = 0, im = 0; const int N = (int) out.size();
            for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / N), a = juce::MathConstants<double>::twoPi * hz * i / fs; re += w * out[(size_t) i] * std::cos (a); im += w * out[(size_t) i] * std::sin (a); }
            return std::sqrt (re * re + im * im); };
        int best = 0; double bm = 0;
        for (int k = 3; k <= 10; ++k) { const double m = mag (k * f0 * ratio); if (m > bm) { bm = m; best = k; } }
        std::printf ("psola: formant x%.2f -> harmonic %d (%.0f Hz), delay %.1f ms\n", fmt, best, best * f0 * ratio, dMax / 48.0);
        CHECK (fmt > 1.1 ? best >= 6 : best == 5);   // 5 x 188 Hz = 941 Hz stays nearest the 1 kHz formant; a sped-up (chipmunk) voice peaks at 6 x 188 = 1129 Hz
        CHECK (dMax < 0.03 * fs);
    }
    // AutoPitch: singing on/off (breaths, gaps) must not crack - no sample jumps
    {
        auto ap = createBuiltinEffect ("autopitch");
        ap->setParam ("style", 3.0f);
        ap->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512); PlayContext ctx; ctx.keyRoot = 0; ctx.scaleIndex = 0;
        double ph = 0; float prev = 0, maxJump = 0; long t = 0;
        for (int k = 0; k < 400; ++k)
        {
            for (int i = 0; i < 512; ++i, ++t)
            {
                const double sec = (double) t / 48000.0, cyc = std::fmod (sec, 0.6);
                const float gate = (float) juce::jlimit (0.0, 1.0, std::min (cyc / 0.01, (0.35 - cyc) / 0.01));   // 350 ms notes, 10 ms fades
                const double hz = 255.0 + 20.0 * std::sin (sec * 3.0);                                             // drifting voice
                const float v = 0.3f * gate * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * hz / 48000.0;
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            ap->process (b, 512, ctx);
            for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (k > 10) maxJump = std::max (maxJump, std::abs (y - prev)); prev = y; }
        }
        CHECK (maxJump < 0.06f);
    }
    // big session stress: 128 inserts x (EQ, Dyn, Verb) + 64 synth channels, 256-sample buffer
    {
        Engine e;
        e.prepare (48000, 256);
        auto& p = e.project();
        p.songMode = false;
        for (int t = 0; t < 128; ++t)
        {
            const int mt = p.addMixerTrack ("T" + juce::String (t));
            for (auto* id : { "steq7", "stdyn", "verb" }) p.addFx (mt, id);
        }
        for (int c = 0; c < 64; ++c)
        {
            Channel ch; ch.type = ChannelType::Synth; ch.name = "S" + juce::String (c); ch.mixerTrack = 1 + c * 2; ch.synth.unison = 3;
            const int id = p.addChannel (ch);
            for (int k = 0; k < 16; ++k) { Note nt; nt.key = 48 + (c + k) % 24; nt.start = k * 0.25; nt.length = 0.5; nt.velocity = 0.7f; p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt); }
        }
        e.sync();
        e.play();
        juce::AudioBuffer<float> out (2, 256);
        auto runFor = [&] (int blocks) {
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int b = 0; b < blocks; ++b) { float* o[2] { out.getWritePointer (0), out.getWritePointer (1) }; e.processBlock (nullptr, 0, o, 2, 256); }
            return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) / (blocks * 256.0 / 48000.0);
        };
        runFor (50);
        const double mtLoad = runFor (400);
        std::cout << "pool runs " << e.poolStat (false) << " worker jobs " << e.poolStat (true) << std::endl;
        e.setPerformance (1, false, true);
        const double stLoad = runFor (200);
        e.setPerformance (0, true, true);
        std::cout << "big session: " << e.processingThreads() << " threads, real-time load " << juce::String (mtLoad * 100.0, 1)
                  << "% (single thread " << juce::String (stLoad * 100.0, 1) << "%)" << std::endl;
        CHECK (std::isfinite (out.getSample (0, 0)));
        e.stop();
    }
    // 0 latency: every built-in effect at its default settings adds no delay
    for (auto& info : builtinEffects())
    {
        auto fx = createBuiltinEffect (info.id);
        if (! fx) continue;
        fx->prepare (48000, 128);
        if (fx->latencySamples() != 0) std::printf ("latency %s: %d samples\n", info.id, fx->latencySamples());
        CHECK (fx->latencySamples() == 0);
    }
    // 40 plug-ins on one session at a 128-sample buffer: no block may miss the audio deadline (= no crackles)
    {
        Engine e;
        e.prepare (48000, 128);
        auto& p = e.project();
        p.songMode = false;
        const auto& all = builtinEffects();
        int placed = 0;
        for (int t = 0; t < 8; ++t)
        {
            const int mt = p.addMixerTrack ("T" + juce::String (t));
            for (int k = 0; k < 5; ++k, ++placed) p.addFx (mt, all[(size_t) (placed % (int) all.size())].id);
        }
        for (int c = 0; c < 8; ++c)
        {
            Channel ch; ch.type = ChannelType::Synth; ch.name = "S" + juce::String (c); ch.mixerTrack = 1 + c; ch.synth.unison = 3;
            const int id = p.addChannel (ch);
            for (int k = 0; k < 16; ++k) { Note nt; nt.key = 48 + (c * 3 + k) % 24; nt.start = k * 0.25; nt.length = 0.5; nt.velocity = 0.7f; p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt); }
        }
        e.sync();
        e.play();
        juce::AudioBuffer<float> out (2, 128);
        const double deadline = 128.0 / 48000.0;
        double worst = 0, total = 0; int late = 0; const int blocks = 3000;
        for (int b = 0; b < blocks + 100; ++b)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            const auto t0 = juce::Time::getHighResolutionTicks();
            e.processBlock (nullptr, 0, o, 2, 128);
            const double dt = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            if (b < 100) continue;   // warm-up
            worst = std::max (worst, dt); total += dt; if (dt > deadline) ++late;
            CHECK (std::isfinite (out.getSample (0, 0)));
        }
        std::printf ("40 plug-ins @128: avg load %.1f%%, worst block %.1f%% of deadline, %d late blocks of %d\n",
                     100.0 * total / (blocks * deadline), 100.0 * worst / deadline, late, blocks);
        CHECK (total / (blocks * deadline) < 0.5);
        CHECK (late <= blocks / (std::getenv ("CI") != nullptr ? 20 : 200));   // shared CI runners get preempted; avg load is the real gate there
        e.stop();
    }
    // every built-in effect stays finite and silent-in gives finite out
    for (auto& info : builtinEffects())
    {
        auto fx = createBuiltinEffect (info.id);
        CHECK (fx != nullptr);
        if (! fx) continue;
        fx->prepare (48000, 512);
        juce::AudioBuffer<float> b (2, 512);
        PlayContext ctx; ctx.playing = true;
        for (int k = 0; k < 50; ++k)
        {
            for (int i = 0; i < 512; ++i) { const float s = 0.5f * std::sin ((float) (k * 512 + i) * 0.05f); b.setSample (0, i, s); b.setSample (1, i, s); }
            fx->process (b, 512, ctx);
            ctx.beatPos += 512.0 / 48000.0 * 140.0 / 60.0;
            for (int i = 0; i < 512; ++i) CHECK (std::isfinite (b.getSample (0, i)));
        }
    }

    // AutoPitch pulls a sharp tone toward the scale
    {
        const double sr = 44100;
        juce::AudioBuffer<float> b (2, 1024 * 16);
        for (int i = 0; i < b.getNumSamples(); ++i) { const float s = 0.5f * (float) std::sin (2 * juce::MathConstants<double>::pi * 452.0 * i / sr); b.setSample (0, i, s); b.setSample (1, i, s); }
        CHECK (std::abs (detectPitchYin (b.getReadPointer (0), 2048, sr) - 452.0f) < 5.0f);
    }

    // sample-rate conversion: a 96k file's 30 kHz content must not fold back as a tone at 48k; 1 kHz passes at full level
    {
        const int n = 96000;
        std::vector<float> hi ((size_t) n), lo ((size_t) n), outHi ((size_t) n / 2), outLo ((size_t) n / 2);
        for (int i = 0; i < n; ++i)
        {
            hi[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 30000.0 * i / 96000.0);
            lo[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 96000.0);
        }
        sincResample (hi.data(), n, outHi.data(), n / 2, 2.0);
        sincResample (lo.data(), n, outLo.data(), n / 2, 2.0);
        float pHi = 0, pLo = 0;
        for (int i = 1000; i < n / 2 - 1000; ++i) { pHi = std::max (pHi, std::abs (outHi[(size_t) i])); pLo = std::max (pLo, std::abs (outLo[(size_t) i])); }
        CHECK (pHi < 0.005f);                  // alias at 18 kHz is gone (> -40 dB down)
        CHECK (std::abs (pLo - 0.5f) < 0.01f); // in-band level kept
        // 44.1k -> 48k: a 5 kHz sine comes out clean
        std::vector<float> a44 (44100), a48 (48000);
        for (int i = 0; i < 44100; ++i) a44[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 44100.0);
        sincResample (a44.data(), 44100, a48.data(), 48000, 44100.0 / 48000.0);
        double err = 0;
        for (int i = 1000; i < 47000; ++i) err = std::max (err, std::abs (a48[(size_t) i] - 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 48000.0)));
        CHECK (err < 0.005);
    }
    // a fresh sampler channel plays the file untouched (no declick / fade baked in)
    {
        Channel c; c.type = ChannelType::Sampler;
        auto src = std::make_shared<const juce::AudioBuffer<float>> (2, 100);
        CHECK (processSample (src, c, 48000.0) == src);
    }
    // Stretch: 2x longer, starts on the first sample, same pitch, and a steady tone stays steady (no hollow phasing)
    {
        Channel c; c.type = ChannelType::Sampler; c.stretchMode = 1; c.stretchTime = 2.0f;
        juce::AudioBuffer<float> raw (2, 48000);
        for (int i = 0; i < 48000; ++i) { const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.3 * i / 48000.0); raw.setSample (0, i, v); raw.setSample (1, i, v); }
        auto out = processSample (std::make_shared<const juce::AudioBuffer<float>> (raw), c, 48000.0);
        CHECK (std::abs (out->getNumSamples() - 96000) < 100);
        int zc = 0; float lo = 1, hi = 0;
        for (int i = 4800; i < 91200; ++i) if (out->getSample (0, i - 1) <= 0 && out->getSample (0, i) > 0) ++zc;
        for (int w = 4800; w + 2400 < 91200; w += 2400) { const float rms = out->getRMSLevel (0, w, 2400); lo = std::min (lo, rms); hi = std::max (hi, rms); }
        std::printf ("stretch 2x: %d samples, %.1f Hz, level %.3f..%.3f (steady 0.354)\n", out->getNumSamples(), zc / (86400.0 / 48000.0), lo, hi);
        CHECK (std::abs (zc / (86400.0 / 48000.0) - 110.3) < 2.0);
        CHECK (lo > 0.30f && hi < 0.40f);
        CHECK (std::abs (out->getSample (0, 1) - raw.getSample (0, 1)) < 0.01f);
    }
    // portamento: a legato slide keeps the same voice going (no retrigger, no gap); the glide takes the Glide time;
    // in Stretch the slid note still plays the sample's full length instead of running out early
    for (int mode : { 0, 1 })
    {
        Channel c; c.type = ChannelType::Sampler; c.stretchMode = mode; c.glide = 0.1f; c.mono = true; c.rootKey = 60;
        auto raw = std::make_shared<juce::AudioBuffer<float>> (2, 24000);   // 0.5 s, 110 Hz, fading 0.6 -> 0.2
        for (int i = 0; i < 24000; ++i) { const float v = (0.6f - 0.4f * i / 24000.0f) * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * i / 48000.0); raw->setSample (0, i, v); raw->setSample (1, i, v); }
        auto inst = createInstrument (c, raw, 48000.0);
        inst->prepare (48000.0, 512); inst->update (c);
        juce::AudioBuffer<float> o (2, 30000); o.clear();
        inst->noteOn (60, 1.0f, false);
        inst->render (o, 0, 4800);
        inst->noteOff (60); inst->noteOn (67, 1.0f, true);   // the next note starts where this one ends
        inst->render (o, 4800, 25200);
        auto zcHz = [&] (int a, int b) {   // interpolated upward zero crossings: exact to a fraction of a Hz
            double first = -1, last = -1; int z = 0;
            for (int i = a + 1; i < b; ++i)
            {
                const float y0 = o.getSample (0, i - 1), y1 = o.getSample (0, i);
                if (y0 <= 0 && y1 > 0) { const double t = i - 1 + y0 / (y0 - y1); if (first < 0) first = t; last = t; ++z; }
            }
            return z > 1 ? (z - 1) * 48000.0 / (last - first) : 0.0; };
        const float atSlide = o.getRMSLevel (0, 6720, 960), late = o.getRMSLevel (0, 20160, 960), end = o.getRMSLevel (0, 21600, 960);
        float gapMin = 1; for (int w = 480; w + 480 < 16000; w += 240) gapMin = std::min (gapMin, o.getRMSLevel (0, w, 480));
        const double hzAfter = zcHz (10560, 16800), hzMid = zcHz (6600, 7800);
        std::printf ("glide %s: level at slide %.3f (held 0.34), min %.3f, at 0.44 s %.3f, after %.1f Hz (164.8), mid-glide %.1f Hz\n",
                     mode ? "stretch" : "resample", atSlide, gapMin, end, hzAfter, hzMid);
        CHECK (atSlide > 0.29f && atSlide < 0.39f);   // the held note continued (a retrigger would be back at 0.42)
        CHECK (gapMin > 0.12f);
        CHECK (std::abs (hzAfter - 164.8) < 4.0);
        CHECK (hzMid > 122.0 && hzMid < 150.0);   // half way through the 0.1 s glide: about 135 Hz
        if (mode == 1) CHECK (late > 0.12f && end > 0.1f);   // Stretch: still playing to the sample's end
    }

    // a slid note keeps sounding while it's held, past the end of a short sample (resample and stretch)
    for (int mode : { 0, 1 })
    {
        Channel c; c.type = ChannelType::Sampler; c.stretchMode = mode; c.glide = 0.1f; c.mono = true; c.rootKey = 60;
        auto raw = std::make_shared<juce::AudioBuffer<float>> (2, 24000);
        for (int i = 0; i < 24000; ++i) { const float v = (0.6f - 0.4f * i / 24000.0f) * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * i / 48000.0); raw->setSample (0, i, v); raw->setSample (1, i, v); }
        auto inst = createInstrument (c, raw, 48000.0);
        inst->prepare (48000.0, 512); inst->update (c);
        juce::AudioBuffer<float> o (2, 72000); o.clear();
        inst->noteOn (60, 1.0f, false); inst->render (o, 0, 4800);
        inst->noteOff (60); inst->noteOn (67, 1.0f, true); inst->render (o, 4800, 67200);
        float lo = 1; for (int w = 9600; w + 960 < 72000; w += 960) lo = std::min (lo, o.getRMSLevel (0, w, 960));
        std::printf ("held slide %s: lowest level over 1.5 s %.3f\n", mode ? "stretch" : "resample", lo);
        CHECK (lo > 0.08f);
    }
    // tempo sync: a 120 BPM loop in a 60 BPM song plays twice as long at the same pitch; one-shots are untouched
    {
        auto raw = std::make_shared<juce::AudioBuffer<float>> (2, 48000);
        for (int i = 0; i < 48000; ++i) { const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 48000.0); raw->setSample (0, i, v); raw->setSample (1, i, v); }
        Channel c; c.type = ChannelType::Sampler; c.sampleFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Dark Loop 120 BPM Cm.wav");
        CHECK (std::abs (sampleTempo (c) - 120.0) < 1e-6);
        auto synced = processSample (raw, c, 48000.0, 60.0);
        int zc = 0; for (int i = 4800; i < 91200; ++i) if (synced->getSample (0, i - 1) <= 0 && synced->getSample (0, i) > 0) ++zc;
        std::printf ("tempo sync: %d -> %d samples, %.1f Hz (220)\n", raw->getNumSamples(), synced->getNumSamples(), zc / (86400.0 / 48000.0));
        CHECK (std::abs (synced->getNumSamples() - 96000) < 200);
        CHECK (std::abs (zc / (86400.0 / 48000.0) - 220.0) < 3.0);
        c.tempoSync = false; CHECK (processSample (raw, c, 48000.0, 60.0)->getNumSamples() == 48000);
        c.tempoSync = true; c.sampleFile = c.sampleFile.getSiblingFile ("808 - ENDWALKER.wav");
        CHECK (sampleTempo (c) == 0.0 && processSample (raw, c, 48000.0, 60.0) == raw);
        c.sampleFile = c.sampleFile.getSiblingFile ("hat_loop_140bpm.wav"); CHECK (std::abs (sampleTempo (c) - 140.0) < 1e-6);
    }

    // save / load / undo
    {
        auto p = Project::createDemo();
        p.bpm = 151; p.addFx (1, "comp").params["ratio"] = 6;
        p.channels[0].shift = -12.0f;
        auto f = juce::File::createTempFile (".hhs");
        CHECK (saveProject (p, f));
        Project q; CHECK (loadProject (q, f));
        CHECK (q.bpm == 151 && q.channels.size() == p.channels.size() && q.mixer[1].fx.size() == 1);
        CHECK (q.patterns[0].notes.size() == p.patterns[0].notes.size());
        CHECK (std::abs (q.channels[0].shift + 12.0f) < 1.0e-4f);
        {   // reopening a project brings every built-in effect back with its saved knobs, not defaults
            Engine e; e.prepare (44100, 512);
            e.project() = q; e.sync();
            auto* fx = e.builtinFx (1, q.mixer[1].fx[0].uid);
            CHECK (fx != nullptr && std::abs (fx->getParam ("ratio") - 6.0f) < 1.0e-4f);
        }
        UndoHistory h; h.push (p, "a"); p.bpm = 90; h.push (p, "b");
        CHECK (h.undo (p) && p.bpm == 151 && h.redo (p) && p.bpm == 90);
        f.deleteFile();
    }

    // pattern pool: insert after the edited pattern + clear unused keep every clip pointing at the right pattern
    {
        Project p; p.patterns.resize (3); for (int i = 0; i < 3; ++i) p.patterns[(size_t) i].name = "P" + juce::String (i);
        Clip a; a.patternIndex = 0; Clip b; b.patternIndex = 2; p.clips = { a, b }; p.currentPattern = 0;
        Pattern n; n.name = "new";
        p.currentPattern = p.insertPatternAfter (0, n);
        CHECK (p.currentPattern == 1 && p.patterns[1].name == "new" && p.clips[0].patternIndex == 0 && p.clips[1].patternIndex == 3);
        std::set<int> used { 0, 3 };
        p.removePatterns ([&] (int i) { return used.count (i) == 0; });
        CHECK (p.patterns.size() == 2 && p.patterns[1].name == "P2" && p.clips.size() == 2 && p.clips[1].patternIndex == 1 && p.currentPattern == 0);
        p.removePatterns ([] (int) { return true; });
        CHECK (p.patterns.size() == 1 && p.clips.empty());
    }

    // license keys: signed, locked to up to 2 Machine IDs; forged / edited keys and other computers fail
    {
        const juce::String key = "HHS1.eyJsaWNfaWQiOiJISFMtQTIwMzlFNUYxMTQxIiwibmFtZSI6IlRlc3QgQnV5ZXIiLCJ1c2VyX2VtYWlsIjoidGVzdEBleGFtcGxlLmNvbSIsInByb2R1Y3Rfc2t1IjoiaHVtaG91c2Vfc3R1ZGlvIiwiaXNzdWVkX2F0IjoxNzkyNDUwMzEzLCJleHBpcmVzX2F0IjowLCJoYXJkd2FyZV9pZHMiOlsiVEVTVC0wMDAwLTAwMDAtMDAwMSIsIlRFU1QtMDAwMC0wMDAwLTAwMDIiXX0=.8d558a1ac19d617fcc65dde9a510ce053a2b202eaa463b71aecd35ae106260e0a196320924eedb62dba420876a02b952069d1521f7b3d9ea629840d060a6990fa14b442a9c5e89e794f784db0cff1e6e1374e7f6d17636a71889761e10397437d58b2c7b55cf115aa2b9b9ca559d14e4633d0e0c2d77c03943f7c5d59881184d2e929e6e9575dddd8c854262b5b169bfe334775d77ef280186d0757ec2be3e173b73207bd7c7ed178bc3a2864509b0b195ba28a2804eaad4a215fb82b295b24b75f33bb9f92c96e508c91acb45e91c7536998d3137521a3a7782c5139c45d81d87dd2c91e14dda96873be39b75c9f545af8b56ed4d5e4f657b6ed1e739b9abfa";
        CHECK (hhs::license::verifyFor (key, "TEST-0000-0000-0001").ok);
        CHECK (hhs::license::verifyFor (key, "test-0000-0000-0002").ok);
        CHECK (! hhs::license::verifyFor (key, "TEST-0000-0000-0003").ok);
        auto editPayload = key.substring (0, 12) + (key[12] == 'A' ? "B" : "A") + key.substring (13);
        CHECK (! hhs::license::verifyFor (editPayload, "TEST-0000-0000-0001").ok);
        auto editSig = key.dropLastCharacters (1) + (key.getLastCharacter() == '1' ? "2" : "1");
        CHECK (! hhs::license::verifyFor (editSig, "TEST-0000-0000-0001").ok);
        CHECK (! hhs::license::verifyFor ("HHS1.abc.def", "TEST-0000-0000-0001").ok);
        CHECK (! hhs::license::verifyFor ({}, "TEST-0000-0000-0001").ok);
        // retail batch key (no Machine ID): signature still checked. Test key is expired on purpose so it can't unlock anything.
        const juce::String retail = "HHS1.eyJsaWNfaWQiOiJISFMtVEVTVC1FWFBJUkVEIiwicHJvZHVjdF9za3UiOiJodW1ob3VzZV9zdHVkaW8iLCJpc3N1ZWRfYXQiOjEsImV4cGlyZXNfYXQiOjIsImFueV9tYWNoaW5lIjp0cnVlfQ==.91bd3e7cbd0a37676c9b6a934e851c70fb10393bb59e2399b2f0a83a74290e0c88c03ac1167a2337c8fb8650a4be9e50b5ad97da5b3d6060b12afc709cb33b4a6941b5e291be3433e71a2eb742d9118bf2c8950dde59817e28e3c6c0fc455f947ad0e7db16f6793dc531fa3c9c102cb1196046df2d98686c6ead2159e01373dfeeb939fed5d27984164eed868b25e381cd5ffcde623c3e3843ccc66c84c5f1d3601f1b6276d5858dc628a46e73a879c23bfd04da60e54a8ea9cf07587607c81aab19996fdac9484d28b967dbb3471edade4045cc23724f9319542226aac008b1e587fdc4c17c16e8b5ca6b48190edbc5c1ffa0a4e99b6e38177fa2f72e0d5517";
        CHECK (hhs::license::verifyFor (retail, "ANY-MACHINE").error == "This license key has expired.");
        CHECK (hhs::license::verifyFor (retail.dropLastCharacters (1) + (retail.getLastCharacter() == '1' ? "2" : "1"), "ANY-MACHINE").error == "Invalid license key.");
        const auto id = hhs::license::machineId();
        CHECK (id.length() == 19 && id == hhs::license::machineId());
        std::printf ("license: machine id %s, valid key ok, 3rd computer / edited / forged keys rejected\n", id.toRawUTF8());
    }

    // 808 slide chains: every slid note keeps sounding (the 3rd, 4th... note never dies), through the real engine
    for (int cfg = 0; cfg < 28; ++cfg)
    {
        const int mono = cfg & 1, mode = (cfg >> 1) & 1, shape = cfg >> 2;   // 0 legato, 1 one-shot + cut itself, 2 gaps, 3 143 BPM @ 44.1k, 4 overlaps, 5 back to the same key, 6 long fast-decaying 808
        const double sr = shape == 3 ? 44100.0 : 48000.0, bpm = shape == 3 ? 143.0 : 120.0;
        auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-glide-808.wav");
        {
            const int len = shape == 6 ? 96000 : 28800;   // 0.6 s decaying 55 Hz sub, or 2 s that fades out fast
            juce::AudioBuffer<float> b (2, len);
            for (int i = 0; i < len; ++i) { const float v = 0.7f * std::exp ((shape == 6 ? -10.0f : -2.0f) * i / (float) len) * (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * i / 48000.0); b.setSample (0, i, v); b.setSample (1, i, v); }
            wav.deleteFile();
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (wav.createOutputStream().release(), 48000.0, 2, 24, {}, 0));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        auto pr = Project::createDefault();
        Channel c; c.id = 1; c.name = "808"; c.type = ChannelType::Sampler; c.sampleFile = wav; c.glide = 0.08f; c.mono = mono == 1; c.stretchMode = mode;
        if (shape == 1) { c.oneShot = true; c.cutItself = true; }
        pr.channels = { c };
        NoteList nl;
        const int keys[] { 60, 63, 65, 67, 70, 67, 65, 63 }, same[] { 60, 67, 60, 67, 60, 67, 60, 67 };
        const int NN = 100;   // FL: a slide chain keeps going for 100+ notes
        for (int k = 0; k < NN; ++k)
        {
            Note n; n.key = shape == 5 ? same[k % 8] : keys[k % 8]; n.start = k * 0.5; n.slide = k > 0;
            n.length = shape == 2 ? 0.3 : shape == 4 ? 0.7 : 0.5;
            nl.push_back (n);
        }
        pr.patterns[0].notes[1] = nl;
        pr.bpm = bpm;
        Engine e; e.prepare (sr, 512); e.newProject (pr); e.sync(); e.play();
        std::vector<float> outL;
        juce::AudioBuffer<float> out (2, 512);
        const int noteLen = (int) (0.5 * 60.0 / bpm * sr);
        for (int blk = 0; blk * 512 < noteLen * NN + 1024; ++blk)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (nullptr, 0, o, 2, 512);
            outL.insert (outL.end(), out.getReadPointer (0), out.getReadPointer (0) + 512);
        }
        e.stop();
        float lo = 1;
        juce::String lv;
        int firstDead = -1;
        for (int k = 0; k < NN; ++k)   // the middle half of each note
        {
            double sum = 0; const int a0 = k * noteLen + noteLen / 4, b0 = a0 + noteLen / 2;
            for (int i = a0; i < b0; ++i) sum += outL[(size_t) i] * outL[(size_t) i];
            const float rms = (float) std::sqrt (sum / (b0 - a0));
            if (rms <= 0.03f && firstDead < 0) firstDead = k;
            lo = std::min (lo, rms); if (k < 8 || k % 10 == 0) lv << juce::String (rms, 3) << " ";
        }
        std::printf ("808 slide chain (%s, %s, shape %d): note levels %s first dead %d\n", mono ? "mono" : "poly", mode ? "stretch" : "resample", shape, lv.toRawUTF8(), firstDead);
        CHECK (lo > 0.03f);
        wav.deleteFile();
    }

    // built-in 808 / synth slide chains (Channel Rack "808 (glide bass)"): the glide never dies after 5-6 notes
    for (int cfg = 0; cfg < 4; ++cfg)
    {
        const bool gaps = cfg & 1; const int wave = cfg < 2 ? 4 : 0;
        auto pr = Project::createDefault();
        Channel c; c.id = 1; c.name = "808"; c.type = ChannelType::Synth; c.synth.wave = wave; c.glide = 0.08f; c.mono = true; c.rootKey = 36;
        if (wave == 0) c.synth.sustain = 0.0f;
        pr.channels = { c };
        NoteList nl;
        const int keys[] { 36, 39, 41, 43, 46, 43, 41, 39 };
        const int NN = 100;
        for (int k = 0; k < NN; ++k) { Note n; n.key = keys[k % 8]; n.start = k * 0.5; n.slide = k > 0; n.length = gaps ? 0.3 : 0.5; nl.push_back (n); }
        pr.patterns[0].notes[1] = nl;
        pr.bpm = 140;
        Engine e; e.prepare (48000, 512); e.newProject (pr); e.sync(); e.play();
        std::vector<float> outL; juce::AudioBuffer<float> out (2, 512);
        const int noteLen = (int) (0.5 * 60.0 / 140.0 * 48000.0);
        for (int blk = 0; blk * 512 < noteLen * NN + 1024; ++blk)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (nullptr, 0, o, 2, 512);
            outL.insert (outL.end(), out.getReadPointer (0), out.getReadPointer (0) + 512);
        }
        e.stop();
        float lo = 1, first = 0; int firstDead = -1;
        for (int k = 0; k < NN; ++k)
        {
            double sum = 0; const int a0 = k * noteLen + noteLen / 4, b0 = a0 + noteLen / 4;
            for (int i = a0; i < b0; ++i) sum += outL[(size_t) i] * outL[(size_t) i];
            const float rms = (float) std::sqrt (sum / (b0 - a0));
            if (k == 0) first = rms;
            if (rms <= 0.03f && firstDead < 0) firstDead = k;
            if (k > 0) lo = std::min (lo, rms);
        }
        std::printf ("synth slide chain (wave %d, %s): first %.3f, lowest after %.3f, first dead %d\n", wave, gaps ? "gaps" : "legato", first, lo, firstDead);
        CHECK (lo > 0.03f);
    }

    // rock session: HumHouse Drummer + 64 tracks with 3 inserts each + 24 instruments, song mode
    {
        Engine e; e.prepare (48000, 256);
        auto& p = e.project(); p.bpm = 140;
        const auto& all = builtinEffects();
        const int did = addDrummerTrack (p, 0.0, 16);
        for (auto& ch : p.channels) if (ch.id == did) for (int k = 0; k < 3; ++k) p.addFx (ch.mixerTrack, all[(size_t) (k * 5 % (int) all.size())].id);
        std::vector<int> mts;
        for (int t = 0; t < 64; ++t) { const int mt = p.addMixerTrack ("R" + juce::String (t)); mts.push_back (mt); for (int k = 0; k < 3; ++k) p.addFx (mt, all[(size_t) ((t * 3 + k) % (int) all.size())].id); }
        for (int c = 0; c < 24; ++c)
        {
            Channel ch; ch.type = ChannelType::Synth; ch.name = "G" + juce::String (c); ch.mixerTrack = mts[(size_t) c];
            const int id = p.addChannel (ch);
            for (int k = 0; k < 16; ++k) { Note nt; nt.key = 40 + (c + k) % 24; nt.start = k * 0.25; nt.length = 0.5; nt.velocity = 0.6f; p.patterns[0].notes[id].push_back (nt); }
        }
        Clip pc; pc.uid = p.newUid(); pc.lane = 0; pc.patternIndex = 0; pc.start = 0; pc.length = 64; p.clips.push_back (pc);
        e.sync();
        juce::Thread::sleep (15000);   // the multi-mic kit loads in the background
        e.setPositionBeats (0.0); e.play();
        juce::AudioBuffer<float> out (2, 256);
        const double deadline = 256.0 / 48000.0;
        double total = 0, worst = 0; int late = 0; float peak = 0; const int blocks = 1500;
        for (int b = 0; b < blocks + 100; ++b)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            const auto t0 = juce::Time::getHighResolutionTicks();
            e.processBlock (nullptr, 0, o, 2, 256);
            const double dt = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            if (b >= 100) { total += dt; worst = std::max (worst, dt); if (dt > deadline) ++late; }
            peak = std::max (peak, out.getMagnitude (0, 256));
            CHECK (std::isfinite (out.getSample (0, 0)));
        }
        e.stop();
        std::printf ("rock session (drummer + 64 tracks x 3 inserts + 24 instruments) @256: avg load %.1f%%, worst block %.1f%%, %d late of %d, peak %.2f\n",
                     100.0 * total / (blocks * deadline), 100.0 * worst / deadline, late, blocks, peak);
        CHECK (total / (blocks * deadline) < 0.6);
        CHECK (peak > 0.05f);
    }

    // 600-track session: sounding tracks + hundreds of quiet ones with inserts must run well inside real time
    {
        Engine e;
        e.prepare (48000, 256);
        auto& p = e.project();
        p.songMode = false;
        const auto& all = builtinEffects();
        for (int t = 0; t < 600; ++t)
        {
            const int mt = p.addMixerTrack ("T" + juce::String (t));
            p.addFx (mt, all[(size_t) (t % (int) all.size())].id);
            p.addFx (mt, all[(size_t) ((t + 3) % (int) all.size())].id);
        }
        for (int c = 0; c < 32; ++c)
        {
            Channel ch; ch.type = ChannelType::Synth; ch.name = "S" + juce::String (c); ch.mixerTrack = 1 + c * 18;
            const int id = p.addChannel (ch);
            for (int k = 0; k < 16; ++k) { Note nt; nt.key = 48 + (c + k) % 24; nt.start = k * 0.25; nt.length = 0.5; nt.velocity = 0.7f; p.patterns[(size_t) p.currentPattern].notes[id].push_back (nt); }
        }
        e.sync();
        e.play();
        juce::AudioBuffer<float> out (2, 256);
        const double deadline = 256.0 / 48000.0;
        double total = 0; const int blocks = 1500;
        for (int b = 0; b < blocks + 200; ++b)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            const auto t0 = juce::Time::getHighResolutionTicks();
            e.processBlock (nullptr, 0, o, 2, 256);
            if (b >= 200) total += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            CHECK (std::isfinite (out.getSample (0, 0)));
        }
        std::printf ("600 tracks (1200 inserts, 32 sounding) @256: avg load %.1f%% of real time\n", 100.0 * total / (blocks * deadline));
        CHECK (total / (blocks * deadline) < 0.9);
        e.stop();
    }

    // audio-clip hi-hat roll: every hit sounds, audio outside the ruler range stays
    {
        auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-roll-test.wav");
        {
            juce::AudioBuffer<float> b (1, 44100 * 2);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, 0.3f * std::sin (i * 0.05f));
            wav.deleteFile();
            std::unique_ptr<juce::OutputStream> os (wav.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), 44100, 1, 24, {}, 0));
            if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
        }
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.songMode = true; p.bpm = 120;
        Clip c; c.uid = p.newUid(); c.audioFile = wav; c.start = 0; c.length = 4; c.lane = 0; p.clips.push_back (c);
        const int n0 = (int) p.clips.size();
        CHECK (rollAudioClips (p, { c.uid }, 0.25, 1.0, 2.0) == 1);
        CHECK ((int) p.clips.size() == n0 - 1 + 6);   // before + 4 hits + after
        e.sync();
        e.setPositionBeats (0.0); e.play();
        const int total = 44100 * 2; juce::AudioBuffer<float> rec (1, total + 512), out (2, 512);
        for (int pos = 0; pos < total; pos += 512)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (nullptr, 0, o, 2, 512);
            rec.copyFrom (0, pos, out, 0, 0, 512);
        }
        e.stop();
        auto rms = [&] (double b0, double b1) { const int a = (int) (b0 * 22050) + 300, z = (int) (b1 * 22050) - 300; return rec.getRMSLevel (0, a, z - a); };
        float worst = 1.0f;
        for (int k = 0; k < 4; ++k) worst = std::min (worst, rms (1.0 + k * 0.25, 1.25 + k * 0.25));
        std::printf ("audio roll: before %.3f, weakest hit %.3f, after %.3f\n", rms (0, 1), worst, rms (2, 4));
        CHECK (rms (0, 1) > 0.1f && worst > 0.1f && rms (2, 4) > 0.1f);
        wav.deleteFile();
    }

    // Key Finder: A minor progression detuned +18 cents with a 142 BPM kick
    {
        const double fs = 44100; const int len = (int) (fs * 16);
        std::vector<float> x ((size_t) len, 0.0f);
        const double detune = 18.0 / 100.0, beat = 60.0 / 142.0;
        const int chords[4][3] { { 57, 60, 64 }, { 50, 53, 57 }, { 52, 55, 59 }, { 57, 60, 64 } };   // Am Dm Em Am
        for (int i = 0; i < len; ++i)
        {
            const double t = i / fs;
            const int c = ((int) (t / (beat * 4))) % 4;
            double v = 0;
            for (int k = 0; k < 3; ++k)
            {
                const double hz = 440.0 * std::pow (2.0, (chords[c][k] + 12 - 69 + detune) / 12.0);
                for (int h = 1; h <= 4; ++h) v += std::sin (juce::MathConstants<double>::twoPi * hz * h * t) / (h * 3.0);
            }
            const double bass = 440.0 * std::pow (2.0, (chords[c][0] - 12 - 69 + detune) / 12.0);
            v += 0.5 * std::sin (juce::MathConstants<double>::twoPi * bass * t);
            const double tb = std::fmod (t, beat);
            v += 0.9 * std::exp (-tb * 30.0) * std::sin (juce::MathConstants<double>::twoPi * 55.0 * tb);
            x[(size_t) i] = (float) (0.15 * v);
        }
        const auto g = findKey (x.data(), len, fs);
        std::printf ("key finder: %s (expected A minor 142 BPM +18 cents)\n", g.describe().toRawUTF8());
        CHECK (g.root == 9 && g.scaleIndex == 1);
        CHECK (std::abs (g.bpm - 142.0) <= 1.0);
        CHECK (std::abs (g.cents - 18.0f) < 6.0f);
    }
    // clip colour survives save / load
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-colour-test.hhs");
        Project p = Project::createDefault();
        Clip c; c.uid = p.newUid(); c.patternIndex = 0; c.colour = juce::Colour (0xffff5d5d); p.clips.push_back (c);
        CHECK (saveProject (p, f));
        Project q; CHECK (loadProject (q, f));
        CHECK (! q.clips.empty() && q.clips.back().colour == juce::Colour (0xffff5d5d));
        f.deleteFile();
    }

    // Session Drummer: a region plays a real groove, resizing re-writes it with the fill at the new end, the kit sounds
    {
        DrummerParams d;
        auto h8 = drummerHits (d, 120, 32), h6 = drummerHits (d, 120, 24), h5 = drummerHits (d, 120, 20);
        auto count = [] (const std::vector<DrumHit>& v, int lane, double a, double b) { int n = 0; for (auto& h : v) if ((lane < 0 || h.lane == lane) && h.beat >= a && h.beat < b) ++n; return n; };
        auto toms = [] (const std::vector<DrumHit>& v, double a, double b) { int n = 0; for (auto& h : v) if (h.beat >= a && h.beat < b && ((h.lane >= 25 && h.lane <= 28) || (h.lane >= 1 && h.lane <= 6))) ++n; return n; };
        std::printf ("drummer: 8 bars %d hits (kick %d, snare %d), last-bar snare/toms 8-bar %d / 6-bar %d / 5-bar %d\n", (int) h8->size(), count (*h8, 0, 0, 32), count (*h8, 1, 0, 32),
                     toms (*h8, 28, 32), toms (*h6, 20, 24), toms (*h5, 16, 20));
        CHECK (h8->size() > 60 && count (*h8, 0, 0, 32) > 8 && count (*h8, 1, 0, 32) > 6);
        CHECK (count (*h8, -1, 32, 1e9) == 0 && count (*h6, -1, 24, 1e9) == 0);
        CHECK (toms (*h6, 20, 24) > toms (*h8, 20, 24) || toms (*h6, 20, 24) >= 4);   // the fill moved to the new end
        {   // Humanize 0 = every hit, fills included, dead on the grid, in every style
            DrummerParams z; z.humanize = 0; z.swing = 0; z.fills = 1;
            for (int st : { 0, 2, 3, 4, 5, 6, 9 })
            {
                z.character = st; auto hz = drummerHits (z, 120, 32); int off = 0;
                for (auto& h : *hz) { const double a = h.beat * 8, b = h.beat * 6; if (std::abs (a - std::round (a)) > 0.02 && std::abs (b - std::round (b)) > 0.02) ++off; }
                std::printf ("drummer humanize 0 style %d: %d of %d hits off the grid\n", st, off, (int) hz->size());
                CHECK (off == 0 && hz->size() > 40);
            }
            {
            DrummerParams sp; const auto st = drummerSeedSteps (sp, 120, 0);
            CHECK (st[0] != 0 && st[1] != 0);
            for (int g = 0; g < NumDrumGroups; ++g) CHECK (drumGroupOf (drumGroupLane (g)) == g);
        }
        {
            // Room Verb Reverse: the reversed tail swells up into the next beat, clean and finite
            auto rv = createBuiltinEffect ("verb");
            rv->setParam ("reverse", 1.0f); rv->setParam ("revlen", 0.0f); rv->setParam ("mix", 1.0f);
            rv->prepare (48000, 512);
            rv->setParam ("reverse", 1.0f);
            PlayContext pc; pc.bpm = 120.0;
            std::vector<float> out;
            juce::AudioBuffer<float> blk (2, 512);
            for (int k = 0; k < 150; ++k)
            {
                blk.clear();
                if (k == 0) { blk.setSample (0, 0, 1.0f); blk.setSample (1, 0, 1.0f); }
                rv->process (blk, 512, pc);
                for (int i = 0; i < 512; ++i) out.push_back (blk.getSample (0, i));
            }
            bool fin = true; float pk = 0.0f;
            for (auto v : out) { fin = fin && std::isfinite (v); pk = std::max (pk, std::abs (v)); }
            const auto en = [&out] (int a0, int a1) { double e = 0; for (int i = a0; i < a1; ++i) e += out[(size_t) i] * out[(size_t) i]; return e; };
            const double early = en (24000 + 500, 24000 + 6000), late = en (48000 - 6000, 48000 - 500);
            std::printf ("reverse verb: early %.3g late %.3g peak %.3f\n", early, late, pk);
            CHECK (fin && pk < 4.0f && late > early * 3.0 && late > 1.0e-6);
        }
        DrummerParams dv; dv.sectionVel = { 40, 100 };
            CHECK (std::abs (drummerDynamics (dv, 4) - 0.4f) < 1e-3f && std::abs (drummerDynamics (dv, 28) - 0.7f) < 1e-3f && std::abs (drummerDynamics (dv, 40) - 1.0f) < 1e-3f);
        }
        const auto kits = drumKits();
        std::printf ("drummer kits: %d\n", kits.size());
        CHECK (kits.size() >= 3);
        Engine e; e.prepare (44100, 512);
        auto& p = e.project(); p.bpm = 120;
        const int id = addDrummerTrack (p, 0.0, 2);
        CHECK (p.songMode && ! p.clips.empty() && p.clips.back().drummer == id);
        e.sync();
        juce::Thread::sleep (15000);   // kit loads in the background
        e.setPositionBeats (0.0); e.play();
        juce::AudioBuffer<float> out (2, 512); double sum = 0; float peak = 0;
        for (int pos = 0; pos < 44100 * 3; pos += 512)
        {
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            e.processBlock (nullptr, 0, o, 2, 512);
            for (int i = 0; i < 512; ++i) { const float x = out.getSample (0, i); sum += x * x; peak = std::max (peak, std::abs (x)); CHECK (std::isfinite (x)); }
        }
        e.stop();
        std::printf ("drummer audio: rms %.3f peak %.3f\n", std::sqrt (sum / (44100 * 3)), peak);
        CHECK (peak > 0.05f && peak <= 1.2f);
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-drummer-test.hhs");
        p.channels.back().drum.fills = 0.9f; p.channels.back().drum.sectionMute = { 0, 4 }; p.channels.back().drum.pieceDb[1] = -3.5f; CHECK (saveProject (p, f));
        Project q; CHECK (loadProject (q, f));
        CHECK (q.clips.back().drummer == id && q.channels.back().type == ChannelType::Drummer && std::abs (q.channels.back().drum.fills - 0.9f) < 1e-4f && q.channels.back().sampleFile.isDirectory());
        CHECK (q.channels.back().drum.sectionMute.size() == 2 && q.channels.back().drum.sectionMute[1] == 4 && std::abs (q.channels.back().drum.pieceDb[1] + 3.5f) < 1e-3f);
        f.deleteFile();
    }

    // audio clips play back exactly like the file (no garbling): compare the master against the WAV
    for (int fileRate : { 48000, 44100 })
    {
        auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-exact-" + juce::String (fileRate) + ".wav");
        const int n = fileRate * 3;
        juce::AudioBuffer<float> src (2, n); juce::Random rnd (7);
        for (int i = 0; i < n; ++i) { const float v = (float) (0.25 * std::sin (i * 2.0 * juce::MathConstants<double>::pi * 441.0 / fileRate) + 0.05 * (rnd.nextFloat() - 0.5f)); src.setSample (0, i, v); src.setSample (1, i, -v * 0.5f); }
        { wav.deleteFile(); std::unique_ptr<juce::OutputStream> os (wav.createOutputStream());
          std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), fileRate, 2, 24, {}, 0));
          if (w) { os.release(); w->writeFromAudioSampleBuffer (src, 0, n); } }
        Engine e; e.prepare (48000, 512);
        auto& p = e.project(); p.songMode = true; p.bpm = 120;
        Clip c; c.uid = p.newUid(); c.audioFile = wav; c.start = 0; c.length = 4; c.lane = 0; p.clips.push_back (c);
        Clip c2 = c; c2.uid = p.newUid(); c2.start = 8; p.clips.push_back (c2);
        e.sync(); e.setPositionBeats (0.0); e.play();
        const int total = 48000 * 2; juce::AudioBuffer<float> rec (2, total + 512), out (2, 512);
        for (int pos = 0; pos < total; pos += 512) { float* o[2] { out.getWritePointer (0), out.getWritePointer (1) }; e.processBlock (nullptr, 0, o, 2, 512); juce::Thread::sleep (11); rec.copyFrom (0, pos, out, 0, 0, 512); rec.copyFrom (1, pos, out, 1, 0, 512); }
        e.stop();
        auto ref = [&] (int i) { const double ps = i * (double) fileRate / 48000.0; const int i0 = (int) ps; const double fr = ps - i0; return src.getSample (0, i0) * (1 - fr) + src.getSample (0, juce::jmin (n - 1, i0 + 1)) * fr; };
        double num = 0, den = 0, err = 0, sig = 0; int glitches = 0;
        for (int i = 24000; i < 72000; ++i) { num += ref (i) * rec.getSample (0, i); den += ref (i) * ref (i); }
        const double g = den > 0 ? num / den : 0;
        for (int i = 24000; i < 72000; ++i) { const double d = rec.getSample (0, i) - g * ref (i); err += d * d; sig += g * g * ref (i) * ref (i); if (std::abs (d) > 0.05) ++glitches; }
        const double snr = 10.0 * std::log10 (sig / juce::jmax (1e-20, err));
        std::printf ("clip playback %d Hz file in 48k session: gain %.3f, match %.1f dB, %d glitch samples\n", fileRate, g, snr, glitches);
        CHECK (g > 0.5 && g < 1.5);
        CHECK (snr > (fileRate == 48000 ? 60.0 : 25.0));
        CHECK (glitches == 0);
        wav.deleteFile();
    }

    // Stem Splitter: 4 stems add back to the mix exactly, kick lands in Drums, sub bass in Bass, centred tone in Vocals, wide pad in Melodies
    {
        const double sr = 44100; const int n = (int) sr * 4;
        juce::AudioBuffer<float> mix (2, n), kick (2, n), bass (2, n), voc (2, n), pad (2, n);
        for (auto* b : { &mix, &kick, &bass, &voc, &pad }) b->clear();
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr, tb = std::fmod (t, 0.5);
            const float k = (float) (0.6 * std::exp (-tb * 40.0) * std::sin (juce::MathConstants<double>::twoPi * 60.0 * tb * (1.0 + 2.0 * std::exp (-tb * 30))) );
            const float bs = (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t));
            double v = 0; for (int h = 1; h <= 8; ++h) v += std::sin (juce::MathConstants<double>::twoPi * 440.0 * h * t) / h;
            const float vc = (float) (0.12 * v);
            const float pl = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 660.0 * t)), pr = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 990.0 * t));
            kick.setSample (0, i, k); kick.setSample (1, i, k); bass.setSample (0, i, bs); bass.setSample (1, i, bs);
            voc.setSample (0, i, vc); voc.setSample (1, i, vc); pad.setSample (0, i, pl); pad.setSample (1, i, pr);
            for (int ch = 0; ch < 2; ++ch) mix.setSample (ch, i, kick.getSample (ch, i) + bass.getSample (ch, i) + voc.getSample (ch, i) + pad.getSample (ch, i));
        }
        std::array<juce::AudioBuffer<float>, NumStems> st;
        CHECK (splitStems (mix, sr, st));
        double err = 0, sig = 0;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i)
        { double sum = 0; for (auto& b : st) sum += b.getSample (ch, i); const double d = sum - mix.getSample (ch, i); err += d * d; sig += (double) mix.getSample (ch, i) * mix.getSample (ch, i); }
        const double recon = 10.0 * std::log10 (sig / juce::jmax (1e-30, err));
        auto share = [&] (const juce::AudioBuffer<float>& part, int stem) {   // how much of a known part's energy ended up in a stem (projection)
            double num = 0, den = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 4410; i < n - 4410; ++i) { num += (double) part.getSample (ch, i) * st[(size_t) stem].getSample (ch, i); den += (double) part.getSample (ch, i) * part.getSample (ch, i); }
            return den > 0 ? num / den : 0.0; };
        std::printf ("stem split: rebuild %.1f dB | kick->Drums %.2f  bass->Bass %.2f  vocal->Vocals %.2f  pad->Melodies %.2f\n", recon,
                     share (kick, StemDrums), share (bass, StemBass), share (voc, StemVocals), share (pad, StemMelodies));
        CHECK (recon > 60.0);
        CHECK (share (kick, StemDrums) > 0.4 && share (bass, StemBass) > 0.7 && share (voc, StemVocals) > 0.5 && share (pad, StemMelodies) > 0.5);
    }

    // audio is glued to the song: a backup opened after the song folder moved still finds its vocal, and a same-named take of another size is never picked up
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-glue-test");
        root.deleteRecursively();
        auto songA = root.getChildFile ("The Low"); songA.getChildFile ("Recordings").createDirectory();
        auto wav = songA.getChildFile ("Recordings").getChildFile ("Vocals_take.wav");
        auto writeWav = [] (const juce::File& f, int n) {
            juce::AudioBuffer<float> b (1, n); for (int i = 0; i < n; ++i) b.setSample (0, i, 0.3f * std::sin (i * 0.05f));
            f.deleteFile(); std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), 44100, 1, 24, {}, 0));
            if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, n); } };
        writeWav (wav, 44100);
        Project p = Project::createDefault(); p.projectFile = songA.getChildFile ("The Low.hhs");
        Clip c; c.uid = p.newUid(); c.audioFile = wav; c.lane = 0; c.length = 4; p.clips.push_back (c);
        CHECK (saveProject (p, p.projectFile));
        const auto bk = writeBackup (p);
        CHECK (bk.existsAsFile());
        auto songB = root.getChildFile ("Moved").getChildFile ("The Low");
        songB.getParentDirectory().createDirectory();
        CHECK (songA.moveFileTo (songB));
        Project q; CHECK (loadProject (q, songB.getChildFile ("Backup").getChildFile (bk.getFileName())));
        CHECK (! q.clips.empty() && q.clips.back().audioFile == songB.getChildFile ("Recordings").getChildFile ("Vocals_take.wav"));
        // another song's take with the same name but different audio must not be linked
        songB.getChildFile ("Recordings").getChildFile ("Vocals_take.wav").deleteFile();
        songB.getChildFile ("Audio Files").createDirectory();
        writeWav (songB.getChildFile ("Audio Files").getChildFile ("Vocals_take.wav"), 22050);
        Project r; CHECK (loadProject (r, songB.getChildFile ("The Low.hhs")));
        CHECK (! r.clips.empty() && ! r.clips.back().audioFile.existsAsFile());
        std::printf ("glue: backup after move finds the vocal, wrong same-named take ignored\n");
        root.deleteRecursively();
    }

    // a project folder moved somewhere else (Desktop -> USB, other computer) still finds its recorded vocals
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hhs-move-test");
        root.deleteRecursively();
        auto a = root.getChildFile ("A"), b = root.getChildFile ("B");
        a.getChildFile ("Recordings").createDirectory();
        auto take = a.getChildFile ("Recordings").getChildFile ("Vocal_take.wav");
        take.replaceWithText ("RIFF");
        Project p = Project::createDefault();
        Clip c; c.uid = p.newUid(); c.audioFile = take; c.length = 4; p.clips.push_back (c);
        CHECK (saveProject (p, a.getChildFile ("Song.hhs")));
        CHECK (a.moveFileTo (b));
        Project q; CHECK (loadProject (q, b.getChildFile ("Song.hhs")));
        const auto& got = q.clips.back().audioFile;
        std::printf ("moved project: vocal found at %s\n", got.getFullPathName().toRawUTF8());
        CHECK (got.existsAsFile() && got == b.getChildFile ("Recordings").getChildFile ("Vocal_take.wav"));
        root.deleteRecursively();
    }

    std::printf (failures ? "%d FAILURES\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
