#include "StemSplit.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <vector>

namespace hhs
{
const char* stemName (int s) { return stemName (StemSetMain, s); }

static const char* const setNames[NumStemSets][8] {
    { "Drums", "Bass", "Vocals", "Melodies" },
    { "Kick", "Snare & Clap", "Hats & Cymbals", "Percs", "Music (no drums)" },
    { "Bass", "Lead & Vocal", "Keys & Plucks", "Pads", "Drums" },
    { "Kick", "Snare & Clap", "Hats & Cymbals", "Percs", "Bass", "Vocals", "Keys & Plucks", "Pads" } };
int stemCount (int set) { static const int n[] { 4, 5, 5, 8 }; return set >= 0 && set < NumStemSets ? n[set] : 0; }
const char* stemName (int set, int s) { return s >= 0 && s < stemCount (set) ? setNames[set][s] : "Stem"; }
const char* stemSetLabel (int set)
{
    static const char* n[] { "Split stems (Drums, Bass, Vocals, Melodies)", "Split drums apart (Kick, Snare & Clap, Hats, Percs)",
                             "Split melodies apart (Bass, Lead & Vocal, Keys & Plucks, Pads)", "Split every part (8 stems: drums + melodies apart)" };
    return set >= 0 && set < NumStemSets ? n[set] : "";
}
juce::uint32 stemColour (int set, int s)
{
    static const juce::uint32 kick = 0xffff5d5d, snare = 0xffff9a3d, hats = 0xfff2e35a, percs = 0xffc77dff, bass = 0xff3d9bff,
                              voc = 0xffffc93d, keys = 0xff4ad991, pads = 0xff3dd6d0, drums = 0xffff5d5d, music = 0xff8aa0b8;
    static const juce::uint32 c[NumStemSets][8] { { drums, bass, voc, keys }, { kick, snare, hats, percs, music },
                                                 { bass, voc, keys, pads, drums }, { kick, snare, hats, percs, bass, voc, keys, pads } };
    return s >= 0 && s < stemCount (set) ? c[set][s] : 0xff8aa0b8;
}

bool splitStems (const juce::AudioBuffer<float>& src, double sr, std::array<juce::AudioBuffer<float>, NumStems>& out,
                 std::atomic<float>* progress, std::atomic<bool>* cancel)
{
    std::vector<juce::AudioBuffer<float>> v;
    if (! splitStems (src, sr, StemSetMain, v, progress, cancel)) return false;
    for (int s = 0; s < NumStems; ++s) out[(size_t) s] = std::move (v[(size_t) s]);
    return true;
}

bool splitStems (const juce::AudioBuffer<float>& src, double sr, int set, std::vector<juce::AudioBuffer<float>>& out,
                 std::atomic<float>* progress, std::atomic<bool>* cancel)
{
    const int S = stemCount (set);
    if (S <= 0) return false;
    constexpr int order = 12, N = 1 << order, hop = N / 4, bins = N / 2 + 1;
    const int len = src.getNumSamples();
    if (len <= 0 || sr <= 0) return false;
    const int nch = 2;
    auto chan = [&] (int ch) { return src.getReadPointer (juce::jmin (ch, src.getNumChannels() - 1)); };
    const int frames = (len + N) / hop + 1;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);   // periodic Hann
    // spectra: [ch][frame][bin] complex
    std::vector<std::vector<std::complex<float>>> spec ((size_t) nch, std::vector<std::complex<float>> ((size_t) frames * bins));
    std::vector<float> mag ((size_t) frames * bins);   // mid magnitude, drives the masks
    std::vector<std::complex<float>> buf ((size_t) N);
    for (int ch = 0; ch < nch; ++ch)
    {
        const float* x = chan (ch);
        std::vector<std::complex<float>> tin ((size_t) N);
        for (int f = 0; f < frames; ++f)
        {
            const int at = f * hop - N / 2;
            for (int i = 0; i < N; ++i) { const int k = at + i; tin[(size_t) i] = { k >= 0 && k < len ? x[k] * win[(size_t) i] : 0.0f, 0.0f }; }
            fft.perform (tin.data(), buf.data(), false);
            std::copy (buf.begin(), buf.begin() + bins, spec[(size_t) ch].begin() + (size_t) f * bins);
            if (cancel != nullptr && cancel->load()) return false;
        }
        if (progress) progress->store (0.15f * (float) (ch + 1));
    }
    for (size_t i = 0; i < mag.size(); ++i) mag[i] = 0.5f * (std::abs (spec[0][i]) + std::abs (spec[1][i]));
    // harmonic / percussive: median along time (sustained) vs along frequency (hits)
    constexpr int mt = 25, mf = 17;   // a longer time median keeps held notes out of the drums
    std::vector<float> harm (mag.size()), perc (mag.size()), tmp (std::max (mt, mf));
    for (int b = 0; b < bins; ++b)
    {
        for (int f = 0; f < frames; ++f)
        {
            int n = 0;
            for (int d = -mt / 2; d <= mt / 2; ++d) { const int g = juce::jlimit (0, frames - 1, f + d); tmp[(size_t) n++] = mag[(size_t) g * bins + b]; }
            std::nth_element (tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
            harm[(size_t) f * bins + b] = tmp[(size_t) n / 2];
        }
        if (cancel != nullptr && cancel->load()) return false;
    }
    if (progress) progress->store (0.5f);
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < bins; ++b)
        {
            int n = 0;
            for (int d = -mf / 2; d <= mf / 2; ++d) { const int g = juce::jlimit (0, bins - 1, b + d); tmp[(size_t) n++] = mag[(size_t) f * bins + g]; }
            std::nth_element (tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
            perc[(size_t) f * bins + b] = tmp[(size_t) n / 2];
        }
    if (progress) progress->store (0.65f);
    // drum parts: per frame "snare-ness" = a broadband mid hit well above the local level, with more mid than sub (a kick is mostly sub)
    std::vector<float> snareF ((size_t) frames, 0.0f);
    {
        std::vector<float> midE ((size_t) frames), lowE ((size_t) frames);
        for (int f = 0; f < frames; ++f)
        {
            double m = 0, l = 0;
            for (int b = 1; b < bins; ++b)
            {
                const size_t i = (size_t) f * bins + b;
                const float hz = (float) (b * sr / N);
                const float p = perc[i] * perc[i];
                if (hz < 120.0f) l += p; else if (hz > 250.0f && hz < 2500.0f) m += p;
            }
            midE[(size_t) f] = (float) m; lowE[(size_t) f] = (float) l;
        }
        std::vector<float> w;
        float hold = 0.0f;
        for (int f = 0; f < frames; ++f)
        {
            w.clear();
            for (int d = -16; d <= 16; ++d) w.push_back (midE[(size_t) juce::jlimit (0, frames - 1, f + d)]);
            std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end());
            const float med = w[w.size() / 2] + 1e-12f;
            const float rise = juce::jlimit (0.0f, 1.0f, (midE[(size_t) f] / med - 1.5f) / 2.5f);
            const float share = midE[(size_t) f] / (midE[(size_t) f] + lowE[(size_t) f] + 1e-12f);
            const float sn = rise * juce::jlimit (0.0f, 1.0f, (share - 0.3f) / 0.3f);
            hold = juce::jmax (sn, hold * 0.62f);   // the snare's body/tail follows the hit
            snareF[(size_t) f] = hold;
        }
    }
    // melody parts: plucked/struck (keys, plucks, guitars) = a bin jumping above its sustained level, held while it rings
    std::vector<float> pluck;
    if (set == StemSetMelodyParts || set == StemSetFull)
    {
        pluck.assign (mag.size(), 0.0f);
        for (int b = 0; b < bins; ++b)
        {
            float hold = 0.0f;
            for (int f = 0; f < frames; ++f)
            {
                const size_t i = (size_t) f * bins + b;
                const float q = mag[i] / (harm[i] + 1e-9f);
                hold = juce::jmax (juce::jlimit (0.0f, 1.0f, (q - 1.1f) / 0.9f), hold * 0.82f);
                pluck[i] = hold;
            }
        }
    }
    if (progress) progress->store (0.7f);
    auto smooth = [] (float x, float a, float b) { return x <= a ? 0.0f : x >= b ? 1.0f : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (x - a) / (b - a)); };
    float m[8] {};
    // masks per bin (sum to 1): drums = percussive share; the tonal rest is split by band, stereo position and attack
    auto masks = [&] (int f, int b)
    {
        const size_t i = (size_t) f * bins + b;
        // cubed (harder than Wiener) masks: each bin goes mostly to one side, so drums and tone bleed less into each other
        const float h3 = harm[i] * harm[i] * harm[i], p3 = perc[i] * perc[i] * perc[i];
        const float drums = (h3 + p3) > 1e-30f ? p3 / (h3 + p3) : 0.0f;
        const float tonal = 1.0f - drums;
        const float hz = (float) (b * sr / N);
        const float lowW = 1.0f - smooth (hz, 150.0f, 260.0f);
        const float l = std::abs (spec[0][i]), r = std::abs (spec[1][i]);
        const float centre = (l + r) > 1e-12f ? 1.0f - std::abs (l - r) / (l + r) : 0.0f;
        const float band = hz < 160.0f || hz > 9000.0f ? 0.0f : hz < 250.0f ? (hz - 160.0f) / 90.0f : hz > 6000.0f ? (9000.0f - hz) / 3000.0f : 1.0f;
        const float bass = tonal * lowW;
        const float vocW = band * std::pow (centre, 8.0f);   // only what sits dead centre
        const float voc = tonal * (1.0f - lowW) * vocW;
        if (set == StemSetMain) { m[0] = drums; m[1] = bass; m[2] = voc; return; }
        const float kickW = 1.0f - smooth (hz, 100.0f, 180.0f), hatW = smooth (hz, 3500.0f, 6500.0f), midW = juce::jmax (0.0f, 1.0f - kickW - hatW);
        const float sn = snareF[(size_t) f];
        const float kick = drums * kickW, snare = drums * (midW * sn + hatW * 0.5f * sn), hats = drums * hatW * (1.0f - 0.5f * sn), percs = drums * midW * (1.0f - sn);
        if (set == StemSetDrumParts) { m[0] = kick; m[1] = snare; m[2] = hats; m[3] = percs; return; }
        const float rest = tonal * (1.0f - lowW) * (1.0f - vocW);
        const float keys = rest * pluck[i];
        if (set == StemSetMelodyParts) { m[0] = bass; m[1] = voc; m[2] = keys; m[3] = rest - keys; return; }
        m[0] = kick; m[1] = snare; m[2] = hats; m[3] = percs; m[4] = bass; m[5] = voc; m[6] = keys;
    };
    // inverse: masked spectra, Hann^2 overlap-add (hop N/4 -> constant 1.5); the last stem is the rest of the mix, so the stems always add back exactly
    out.assign ((size_t) S, {});
    for (auto& o : out) { o.setSize (nch, len); o.clear(); }
    {
        std::vector<std::vector<std::complex<float>>> fin ((size_t) (S - 1), std::vector<std::complex<float>> ((size_t) N));
        std::vector<std::complex<float>> tout ((size_t) N);
        for (int ch = 0; ch < nch; ++ch)
        {
            for (int f = 0; f < frames; ++f)
            {
                const size_t o = (size_t) f * bins;
                for (int b = 0; b < bins; ++b)
                {
                    masks (f, b);
                    for (int s = 0; s < S - 1; ++s) fin[(size_t) s][(size_t) b] = spec[(size_t) ch][o + (size_t) b] * m[s];
                }
                const int at = f * hop - N / 2;
                for (int s = 0; s < S - 1; ++s)
                {
                    auto& fs = fin[(size_t) s];
                    for (int b = 1; b < N / 2; ++b) fs[(size_t) (N - b)] = std::conj (fs[(size_t) b]);
                    fft.perform (fs.data(), tout.data(), true);
                    float* y = out[(size_t) s].getWritePointer (ch);
                    for (int i = 0; i < N; ++i) { const int k = at + i; if (k >= 0 && k < len) y[k] += tout[(size_t) i].real() * win[(size_t) i] / 1.5f; }
                }
                if ((f & 63) == 0)
                {
                    if (cancel != nullptr && cancel->load()) return false;
                    if (progress) progress->store (0.7f + 0.28f * (float) (ch * frames + f) / (float) (nch * frames));
                }
            }
        }
    }
    for (int ch = 0; ch < nch; ++ch)
    {
        float* rest = out[(size_t) S - 1].getWritePointer (ch);
        const float* x = chan (ch);
        for (int i = 0; i < len; ++i)
        {
            float acc = x[i];
            for (int s = 0; s < S - 1; ++s) acc -= out[(size_t) s].getSample (ch, i);
            rest[i] = acc;
        }
    }
    if (src.getNumChannels() == 1) for (auto& b : out) { b.addFrom (0, 0, b, 1, 0, len); b.applyGain (0.5f); b.setSize (1, len, true); }
    if (progress) progress->store (1.0f);
    return true;
}

std::array<juce::File, NumStems> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir,
                                                    std::atomic<float>* progress, std::atomic<bool>* cancel)
{
    std::array<juce::File, NumStems> files {};
    const auto v = splitStemsToFiles (in, startSec, lengthSec, outDir, StemSetMain, progress, cancel);
    if (v.size() == (size_t) NumStems) for (int s = 0; s < NumStems; ++s) files[(size_t) s] = v[(size_t) s];
    return files;
}

std::vector<juce::File> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir, int set,
                                           std::atomic<float>* progress, std::atomic<bool>* cancel)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (in));
    if (r == nullptr || r->lengthInSamples <= 0) return {};
    const auto a = (juce::int64) juce::jmax (0.0, startSec * r->sampleRate);
    const auto n = (int) juce::jmin ((juce::int64) (lengthSec * r->sampleRate), r->lengthInSamples - a, (juce::int64) (r->sampleRate * 60 * 12));
    if (n <= 0) return {};
    juce::AudioBuffer<float> src ((int) juce::jmax (1u, juce::jmin (2u, r->numChannels)), n);
    r->read (&src, 0, n, a, true, true);
    std::vector<juce::AudioBuffer<float>> out;
    if (! splitStems (src, r->sampleRate, set, out, progress, cancel)) return {};
    outDir.createDirectory();
    std::vector<juce::File> files;
    for (int s = 0; s < (int) out.size(); ++s)
    {
        auto f = outDir.getNonexistentChildFile (in.getFileNameWithoutExtension() + " - " + juce::String (stemName (set, s)).replace ("&", "and"), ".wav");
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), r->sampleRate, (unsigned) out[(size_t) s].getNumChannels(), 32, {}, 0));
        if (w == nullptr) return {};
        os.release();
        w->writeFromAudioSampleBuffer (out[(size_t) s], 0, n);
        files.push_back (f);
    }
    return files;
}
}
