#include <set>
#include "../Source/Engine.h"
#include "../Source/NoteTools.h"
#include "../Source/ProjectIO.h"
#include "../Source/Effects.h"
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
    ap->setParam ("style", (float) style); ap->setParam ("speed", speed); ap->setParam ("natvib", natvib); ap->setParam ("snap", snap);
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
    const int lag = ap->latencySamples() + (style == 3 ? 72 : 300);   // shifter delay (approx)
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
    ap->setParam ("style", (float) style); ap->setParam ("speed", speed);
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
    if (std::getenv ("HHS_AP_PROBE2") != nullptr)
    {
        for (double m : { 57.6, 56.6, 64.3, 50.5 })
            for (int st = 0; st < 4; ++st)
            {
                for (int ab = 0; ab < 1; ++ab)
                {
                    if (st == 1 || st == 2) continue;
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
    // PSOLA (Retune / Humanize styles): pitch moves, the voice's tone (formant) stays put - no chipmunk
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
        std::printf ("psola: formant harmonic %d (%.0f Hz, tone kept at ~1 kHz), delay %.1f ms\n", best, best * f0 * ratio, dMax / 48.0);
        CHECK (best == 5);   // 5 x 188 Hz = 941 Hz stays nearest the 1 kHz formant; a sped-up (chipmunk) voice peaks at 6 x 188 = 1129 Hz
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
        CHECK (late <= blocks / 200);
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

    std::printf (failures ? "%d FAILURES\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
