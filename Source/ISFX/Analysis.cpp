#include "Analysis.h"
#include "Media.h"
#include <juce_dsp/juce_dsp.h>
#include <numeric>
#include <algorithm>
#include <iostream>

namespace isfx
{
static constexpr int kFftOrder = 10, kFft = 1 << kFftOrder, kHop = 512;

OnsetEnvelope computeOnsets (const std::vector<float>& x, int rate)
{
    OnsetEnvelope env; env.hopSec = (double) kHop / rate;
    if (x.size() < (size_t) kFft) return env;
    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> win (kFft), buf (2 * kFft), prev (kFft / 2 + 1, 0.0f), mag (kFft / 2 + 1);
    for (int i = 0; i < kFft; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (kFft - 1));
    const double binHz = (double) rate / kFft;
    const int lowMax = (int) (150 / binHz), midMax = (int) (4000 / binHz), highMin = (int) (6000 / binHz);
    size_t frames = (x.size() - kFft) / kHop + 1;
    env.all.resize (frames); env.low.resize (frames); env.mid.resize (frames); env.high.resize (frames); env.rms.resize (frames);
    for (size_t f = 0; f < frames; ++f)
    {
        const float* s = x.data() + f * kHop;
        double e = 0;
        for (int i = 0; i < kFft; ++i) { buf[(size_t) i] = s[i] * win[(size_t) i]; e += s[i] * s[i]; }
        std::fill (buf.begin() + kFft, buf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        float a = 0, l = 0, m = 0, h = 0;
        for (int b = 1; b <= kFft / 2; ++b)
        {
            mag[(size_t) b] = std::log1p (100.0f * buf[(size_t) b]);
            float d = std::max (0.0f, mag[(size_t) b] - prev[(size_t) b]);
            a += d;
            if (b <= lowMax) l += d; else if (b <= midMax) m += d; else if (b >= highMin) h += d;
        }
        std::swap (prev, mag);
        env.all[f] = a; env.low[f] = l; env.mid[f] = m; env.high[f] = h;
        env.rms[f] = (float) std::sqrt (e / kFft);
    }
    return env;
}

static float meanOf (const std::vector<float>& v) { return v.empty() ? 0.0f : std::accumulate (v.begin(), v.end(), 0.0f) / (float) v.size(); }

// sum of envelope (linearly interpolated) at t0 + k*period
static double combScore (const std::vector<float>& e, double hop, double period, double t0)
{
    double s = 0; int n = 0;
    double total = (double) e.size() * hop;
    for (double t = t0; t < total - hop; t += period)
    {
        double fi = t / hop; size_t i = (size_t) fi; double fr = fi - (double) i;
        if (i + 1 >= e.size()) break;
        s += e[i] * (1 - fr) + e[i + 1] * fr; ++n;
    }
    return n > 0 ? s / n : 0;
}

static std::vector<size_t> pickPeaks (const std::vector<float>& e, double hop, double minGapSec, float k)
{
    std::vector<size_t> out;
    const int w = std::max (3, (int) (0.25 / hop));
    size_t gap = (size_t) std::max (1.0, minGapSec / hop);
    for (size_t i = 1; i + 1 < e.size(); ++i)
    {
        if (e[i] < e[i - 1] || e[i] < e[i + 1]) continue;
        size_t a = i > (size_t) w ? i - (size_t) w : 0, b = std::min (e.size(), i + (size_t) w + 1);
        double mean = 0, sq = 0;
        for (size_t j = a; j < b; ++j) { mean += e[j]; sq += (double) e[j] * e[j]; }
        mean /= (double) (b - a); double sd = std::sqrt (std::max (0.0, sq / (double) (b - a) - mean * mean));
        if (e[i] < mean + k * sd || e[i] <= 1e-3f) continue;
        if (! out.empty() && i - out.back() < gap) { if (e[i] > e[out.back()]) out.back() = i; continue; }
        out.push_back (i);
    }
    return out;
}

static void computeSections (SongAnalysis& a, const OnsetEnvelope& env)
{
    a.barEnergy.clear(); a.barSection.clear();
    double barSec = a.beatSec() * a.beatsPerBar;
    int bars = (int) std::ceil (ticksToSec (a.duration - a.gridOffset) / barSec);
    if (bars <= 0 || env.rms.empty()) return;
    for (int b = 0; b < bars; ++b)
    {
        double t0 = ticksToSec (a.gridOffset) + b * barSec, t1 = t0 + barSec;
        size_t i0 = (size_t) std::max (0.0, t0 / env.hopSec), i1 = std::min (env.rms.size(), (size_t) std::max (0.0, t1 / env.hopSec));
        double s = 0, o = 0; int n = 0;
        for (size_t i = i0; i < i1; ++i) { s += env.rms[i]; o += env.all[i]; ++n; }
        a.barEnergy.push_back (n ? (float) (s / n) : 0.0f);
    }
    // smooth over 2 bars, normalize, 3-level sections via tertiles
    auto e = a.barEnergy;
    for (size_t i = 0; i < e.size(); ++i)
        e[i] = (a.barEnergy[i] + a.barEnergy[i > 0 ? i - 1 : i] + a.barEnergy[std::min (e.size() - 1, i + 1)]) / 3.0f;
    float mx = *std::max_element (e.begin(), e.end());
    for (auto& v : e) v = mx > 0 ? v / mx : 0;
    a.barEnergy = e;
    auto sorted = e; std::sort (sorted.begin(), sorted.end());
    float t1 = sorted[sorted.size() / 3], t2 = sorted[sorted.size() * 2 / 3];
    for (auto v : e) a.barSection.push_back (v >= t2 && v > 0.6f ? 2 : (v <= t1 ? 0 : 1));
}

static OnsetEnvelope g_lastEnv; // not used across threads concurrently for writes; only for reapplyGrid convenience

SongAnalysis analyzeSong (const std::vector<float>& x, int rate)
{
    SongAnalysis a;
    a.duration = secToTicks ((double) x.size() / rate);
    auto env = computeOnsets (x, rate);
    if (env.all.size() < 64) return a;
    const double hop = env.hopSec;

    // --- tempo: harmonic-summed autocorrelation (T + T/2-octave check) with a log-gaussian prior around 120 BPM,
    //     computed on 20 s windows; the median window tempo wins (robust to tempo changes and intros)
    auto e = env.all;
    float m = meanOf (e); for (auto& v : e) v = std::max (0.0f, v - m);
    auto acAt = [&] (size_t i0, size_t i1, double lag)
    {
        double ac = 0; size_t L = (size_t) lag; double fr = lag - (double) L;
        for (size_t i = i0; i + L + 1 < i1; ++i) ac += e[i] * (e[i + L] * (1 - fr) + e[i + L + 1] * fr);
        return ac;
    };
    auto tempoOf = [&] (size_t i0, size_t i1)
    {
        double best = -1, period = 0.5;
        for (double bpm = 60; bpm <= 200; bpm += 0.25)
        {
            double lag = 60.0 / bpm / hop;
            double sc = acAt (i0, i1, lag) + 0.5 * acAt (i0, i1, lag * 2) + 0.25 * acAt (i0, i1, lag * 4);
            sc *= std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2));
            if (sc > best) { best = sc; period = 60.0 / bpm; }
        }
        return period;
    };
    std::vector<double> periods;
    const size_t win = (size_t) (20.0 / hop);
    if (e.size() <= win * 3 / 2) periods.push_back (tempoOf (0, e.size()));
    else for (size_t i0 = 0; i0 + win / 2 < e.size(); i0 += win) periods.push_back (tempoOf (i0, std::min (e.size(), i0 + win)));
    std::sort (periods.begin(), periods.end());
    double bestPeriod = periods[periods.size() / 2];
    double bestScore = -1;
    // --- fine tempo + phase via comb fit over whole song
    double bestP = bestPeriod, bestPhase = 0; bestScore = -1;
    for (double p = bestPeriod * 0.985; p <= bestPeriod * 1.015; p += bestPeriod * 0.0005)
        for (double ph = 0; ph < p; ph += hop / 2)
        {
            double s = combScore (e, hop, p, ph);
            if (s > bestScore) { bestScore = s; bestP = p; bestPhase = ph; }
        }
    a.bpm = std::round (60.0 / bestP * 100.0) / 100.0;
    if (std::abs (a.bpm - std::round (a.bpm)) < 0.15) a.bpm = std::round (a.bpm);
    bestP = 60.0 / a.bpm;
    // re-fit phase at the rounded tempo
    bestScore = -1;
    for (double ph = 0; ph < bestP; ph += hop / 4)
    {
        double s = combScore (e, hop, bestP, ph);
        if (s > bestScore) { bestScore = s; bestPhase = ph; }
    }
    // --- drum events
    auto addEvents = [&] (const std::vector<float>& band, EventType type, double gap, float k)
    {
        float mx = band.empty() ? 1.0f : *std::max_element (band.begin(), band.end());
        for (auto i : pickPeaks (band, hop, gap, k))
        {
            // onset time: centre of the analysis window that first sees the new energy
            double t = (double) i * hop + (double) kFft / rate * 0.5 - hop * 0.5;
            float st = mx > 0 ? band[i] / mx : 0;
            a.events.push_back ({ secToTicks (std::max (0.0, t)), type, st, juce::jlimit (0.0f, 1.0f, st * 2.0f) });
        }
    };
    addEvents (env.low, EventType::Kick, 0.12, 1.2f);
    addEvents (env.high, EventType::HiHat, 0.06, 0.9f);
    // snare: mid-band peaks that are not kicks (mid dominates low at that instant)
    {
        std::vector<float> sn (env.mid.size());
        float ml = meanOf (env.low) + 1e-6f, mm = meanOf (env.mid) + 1e-6f;
        for (size_t i = 0; i < sn.size(); ++i)
        {
            float lowN = env.low[i] / ml, midN = env.mid[i] / mm;
            sn[i] = midN > lowN * 0.9f ? env.mid[i] : 0.0f;
        }
        addEvents (sn, EventType::Snare, 0.15, 1.5f);
    }
    std::sort (a.events.begin(), a.events.end(), [] (auto& p, auto& q) { return p.time < q.time; });
    {
        // beat phase = circular mean of strong kick/snare hits modulo the beat (same time base as the events)
        double sx = 0, sy = 0;
        for (auto& ev : a.events)
            if (ev.type != EventType::HiHat)
            {
                double ph = std::fmod (ticksToSec (ev.time), bestP) / bestP * juce::MathConstants<double>::twoPi;
                sx += ev.strength * std::cos (ph); sy += ev.strength * std::sin (ph);
            }
        if (sx != 0 || sy != 0)
        {
            double ph = std::atan2 (sy, sx); if (ph < 0) ph += juce::MathConstants<double>::twoPi;
            bestPhase = ph / juce::MathConstants<double>::twoPi * bestP;
        }
        else bestPhase += (double) kFft / rate * 0.5 - hop * 0.5;
        // downbeat: the beat phase (of 4) with the most kick energy
        int bestDb = 0; double dbScore = -1;
        for (int k = 0; k < 4; ++k)
        {
            double sc = 0;
            for (auto& ev : a.events)
                if (ev.type == EventType::Kick)
                {
                    double b = (ticksToSec (ev.time) - bestPhase - k * bestP) / bestP;
                    double fr = b / 4.0 - std::floor (b / 4.0);
                    if (fr < 0.03 || fr > 0.97) sc += ev.strength;
                }
            if (sc > dbScore) { dbScore = sc; bestDb = k; }
        }
        double off = std::fmod (bestPhase + bestDb * bestP, bestP * 4);
        a.gridOffset = secToTicks (off);
    }
    computeSections (a, env);
    a.valid = true;
    g_lastEnv = std::move (env);
    return a;
}

void reapplyGrid (SongAnalysis& a, double bpm, Ticks offset)
{
    a.bpm = juce::jlimit (40.0, 300.0, bpm);
    a.gridOffset = offset;
    if (! g_lastEnv.rms.empty()) computeSections (a, g_lastEnv);
}

// ------------------------------------------------------------------ sync
// Per-frame log energy in 24 log-spaced bands (100 Hz - 9 kHz), z-scored per band. Captures melody/harmony,
// not just drum onsets, so repetitive drum loops still correlate to a single unique position.
static std::vector<float> normalizedEnv (const std::vector<float>& x, int rate)
{
    auto env = computeOnsets (x, rate).all;
    float m = meanOf (env);
    double sd = 0; for (auto v : env) sd += (v - m) * (v - m);
    sd = std::sqrt (sd / std::max<size_t> (1, env.size())) + 1e-9;
    for (auto& v : env) v = (float) ((v - m) / sd);
    return env;
}

static std::vector<std::vector<float>> bandFeatures (const std::vector<float>& x, int rate)
{
    constexpr int nb = 24;
    std::vector<std::vector<float>> out (nb);
    if (x.size() < (size_t) kFft) return out;
    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> win (kFft), buf (2 * kFft);
    for (int i = 0; i < kFft; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (kFft - 1));
    int edges[nb + 1];
    for (int k = 0; k <= nb; ++k) edges[k] = std::max (1, (int) (100.0 * std::pow (90.0, (double) k / nb) / ((double) rate / kFft)));
    size_t frames = (x.size() - kFft) / kHop + 1;
    for (auto& v : out) v.resize (frames);
    for (size_t f = 0; f < frames; ++f)
    {
        const float* s = x.data() + f * kHop;
        for (int i = 0; i < kFft; ++i) buf[(size_t) i] = s[i] * win[(size_t) i];
        std::fill (buf.begin() + kFft, buf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k < nb; ++k)
        {
            double e = 1e-9;
            for (int bn = edges[k]; bn < std::max (edges[k] + 1, edges[k + 1]) && bn <= kFft / 2; ++bn) e += buf[(size_t) bn] * buf[(size_t) bn];
            out[(size_t) k][f] = (float) std::log (e);
        }
    }
    for (auto& v : out)
    {
        // remove slow trend (1 s moving average) so level differences between mic and master don't matter
        std::vector<float> sm (v.size());
        const int w = (int) (0.5 * rate / kHop);
        double acc = 0; size_t lo = 0, hi = 0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            while (hi < v.size() && hi <= i + (size_t) w) acc += v[hi++];
            while (lo + (size_t) w < i) acc -= v[lo++];
            sm[i] = (float) (acc / (double) (hi - lo));
        }
        double m2 = 0; for (size_t i = 0; i < v.size(); ++i) { v[i] -= sm[i]; m2 += v[i] * v[i]; }
        float sd = (float) std::sqrt (m2 / std::max<size_t> (1, v.size())) + 1e-6f;
        for (auto& q : v) q /= sd;
    }
    return out;
}

SyncResult syncSegment (const std::vector<float>& song, const std::vector<float>& a, int rate, Ticks srcIn, Ticks len, double minContext)
{
    const double total = (double) a.size() / rate, s0 = ticksToSec (srcIn), l = ticksToSec (len);
    const double w = std::min (total, std::max (minContext, l));
    const double ws = juce::jlimit (0.0, std::max (0.0, total - w), s0 + l * 0.5 - w * 0.5);
    const size_t i0 = (size_t) (ws * rate), i1 = std::min (a.size(), (size_t) ((ws + w) * rate));
    SyncResult r;
    if (i1 <= i0) { r.note = "no audio"; return r; }
    std::vector<float> piece (a.begin() + (std::ptrdiff_t) i0, a.begin() + (std::ptrdiff_t) i1);
    r = syncToSong (song, piece, rate);
    r.offset -= secToTicks ((double) i0 / rate);
    return r;
}

SyncResult syncToSong (const std::vector<float>& song, const std::vector<float>& take, int rate)
{
    SyncResult r;
    if (take.size() < (size_t) rate || song.size() < (size_t) rate) { r.note = "Audio too short to sync"; return r; }
    auto fa = bandFeatures (song, rate), fb = bandFeatures (take, rate);
    auto ea = normalizedEnv (song, rate), eb = normalizedEnv (take, rate);
    fa.push_back (ea); fb.push_back (eb);
    const auto& a = fa[0]; const auto& b = fb[0];
    const double hop = (double) kHop / rate;
    size_t n = a.size() + b.size();
    int order = 1; while ((size_t) (1 << order) < n) ++order;
    size_t N = (size_t) 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> A (N), B (N), FA (N), FB (N), Acc (N), C (N);
    for (size_t k = 0; k < fa.size(); ++k)
    {
        std::fill (A.begin(), A.end(), std::complex<float>()); std::fill (B.begin(), B.end(), std::complex<float>());
        for (size_t i = 0; i < fa[k].size(); ++i) A[i] = fa[k][i];
        for (size_t i = 0; i < fb[k].size(); ++i) B[i] = fb[k][i];
        fft.perform (A.data(), FA.data(), false); fft.perform (B.data(), FB.data(), false);
        for (size_t i = 0; i < N; ++i) Acc[i] += FA[i] * std::conj (FB[i]);
    }
    fft.perform (Acc.data(), C.data(), true);
    // lag L (in frames): song[i + L] ~ take[i]; negative lags wrap to the end
    std::vector<float> corr (N);
    for (size_t i = 0; i < N; ++i) corr[i] = C[i].real();
    long best = 0; float bv = -1e30f;
    long minLag = -(long) b.size() + (long) (1.0 / hop), maxLag = (long) a.size() - (long) (1.0 / hop);
    auto at = [&] (long L) { return corr[(size_t) ((L % (long) N + (long) N) % (long) N)]; };
    for (long L = minLag; L <= maxLag; ++L) if (at (L) > bv) { bv = at (L); best = L; }
    // overlap-normalized prominence vs the rest of the correlation
    double mean = 0, sq = 0; long cnt = 0; float second = -1e30f;
    long excl = (long) (0.4 / hop);
    for (long L = minLag; L <= maxLag; ++L)
    {
        float v = at (L); mean += v; sq += (double) v * v; ++cnt;
        if (std::abs (L - best) > excl) second = std::max (second, v);
    }
    mean /= std::max (1L, cnt); double sd = std::sqrt (std::max (0.0, sq / std::max (1L, cnt) - mean * mean)) + 1e-9;
    double z = (bv - mean) / sd, ratio = second > 0 ? bv / second : 3.0;
    // a true match stands clearly above every other alignment (ratio); z guards against flat/noisy correlations
    r.confidence = (float) (juce::jlimit (0.0, 1.0, (ratio - 1.05) / 0.2) * juce::jlimit (0.0, 1.0, (z - 2.5) / 2.0));

    // sample-accurate refinement with raw audio around the frame lag
    double coarse = best * hop;
    long cS = (long) std::llround (coarse * rate);
    long bestS = cS; double bestC = -1e30;
    size_t win = std::min<size_t> (take.size(), (size_t) rate * 8);
    size_t t0 = take.size() > win ? (take.size() - win) / 2 : 0;
    for (long d = -kHop; d <= kHop; d += 2)
    {
        long L = cS + d; double s = 0;
        for (size_t i = t0; i < t0 + win; i += 2)
        {
            long j = (long) i + L;
            if (j < 0 || j >= (long) song.size()) continue;
            s += take[i] * song[(size_t) j];
        }
        if (s > bestC) { bestC = s; bestS = L; }
    }
    r.offset = secToTicks ((double) bestS / rate);
    r.ok = r.confidence >= 0.35f;
    r.note = r.ok ? juce::String ("Matched to song at ") + juce::String (ticksToSec (r.offset), 3) + " s"
                  : juce::String ("Low confidence match - check and place manually");
    return r;
}

float scoreShotQuality (const juce::File& video)
{
    VideoDecoder d; juce::String err;
    if (! d.open (video, 180, err)) return 0.5f;
    auto pr = probeMedia (video);
    double dur = ticksToSec (pr.duration);
    double sharp = 0, expo = 0; int n = 0;
    for (int k = 1; k <= 5; ++k)
    {
        Frame f;
        if (! d.frameAt (secToTicks (dur * k / 6.0), f)) continue;
        double lap = 0, lum = 0; int c = 0;
        for (int y = 1; y < f.h - 1; y += 2)
            for (int x = 1; x < f.w - 1; x += 2)
            {
                auto L = [&] (int xx, int yy) { auto* p = &f.rgba[(size_t) ((yy * f.w + xx) * 4)]; return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; };
                double v = L (x, y);
                lap += std::abs (4 * v - L (x - 1, y) - L (x + 1, y) - L (x, y - 1) - L (x, y + 1));
                lum += v; ++c;
            }
        if (c == 0) continue;
        sharp += lap / c; expo += 1.0 - std::abs (lum / c / 255.0 - 0.45) * 2.0; ++n;
    }
    if (n == 0) return 0.5f;
    return (float) juce::jlimit (0.0, 1.0, 0.6 * juce::jlimit (0.0, 1.0, sharp / n / 12.0) + 0.4 * expo / n);
}
}
