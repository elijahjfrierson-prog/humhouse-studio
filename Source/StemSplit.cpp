#include "StemSplit.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <vector>

namespace hhs
{
const char* stemName (int s)
{
    static const char* n[] { "Drums", "Bass", "Vocals", "Melodies" };
    return s >= 0 && s < NumStems ? n[s] : "Stem";
}

bool splitStems (const juce::AudioBuffer<float>& src, double sr, std::array<juce::AudioBuffer<float>, NumStems>& out,
                 std::atomic<float>* progress, std::atomic<bool>* cancel)
{
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
    constexpr int mt = 17, mf = 17;
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
    // masks per bin: drums = percussive share; the tonal rest is split by band and stereo position
    std::array<std::vector<float>, NumStems> mask;
    for (auto& m : mask) m.resize (mag.size());
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < bins; ++b)
        {
            const size_t i = (size_t) f * bins + b;
            const float h2 = harm[i] * harm[i], p2 = perc[i] * perc[i];
            const float drums = (h2 + p2) > 1e-20f ? p2 / (h2 + p2) : 0.0f;
            const float tonal = 1.0f - drums;
            const float hz = (float) (b * sr / N);
            const float lowW = hz < 150.0f ? 1.0f : hz > 260.0f ? 0.0f : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (hz - 150.0f) / 110.0f);
            const float l = std::abs (spec[0][i]), r = std::abs (spec[1][i]);
            const float centre = (l + r) > 1e-12f ? 1.0f - std::abs (l - r) / (l + r) : 0.0f;
            const float band = hz < 160.0f || hz > 9000.0f ? 0.0f : hz < 250.0f ? (hz - 160.0f) / 90.0f : hz > 6000.0f ? (9000.0f - hz) / 3000.0f : 1.0f;
            const float bass = tonal * lowW;
            const float voc = tonal * (1.0f - lowW) * band * std::pow (centre, 6.0f);
            mask[StemDrums][i] = drums; mask[StemBass][i] = bass; mask[StemVocals][i] = voc;
            mask[StemMelodies][i] = juce::jmax (0.0f, 1.0f - drums - bass - voc);
        }
    if (progress) progress->store (0.75f);
    // inverse: masked spectra, Hann^2 overlap-add (hop N/4 -> constant 1.5)
    for (int s = 0; s < NumStems; ++s)
    {
        out[(size_t) s].setSize (nch, len); out[(size_t) s].clear();
        if (s == StemMelodies) continue;   // the rest of the mix, below: the 4 stems always add back to the original exactly
        for (int ch = 0; ch < nch; ++ch)
        {
            float* y = out[(size_t) s].getWritePointer (ch);
            std::vector<std::complex<float>> fin ((size_t) N), tout ((size_t) N);
            for (int f = 0; f < frames; ++f)
            {
                const size_t o = (size_t) f * bins;
                for (int b = 0; b < bins; ++b) fin[(size_t) b] = spec[(size_t) ch][o + (size_t) b] * mask[(size_t) s][o + (size_t) b];
                for (int b = 1; b < N / 2; ++b) fin[(size_t) (N - b)] = std::conj (fin[(size_t) b]);
                fft.perform (fin.data(), tout.data(), true);
                const int at = f * hop - N / 2;
                for (int i = 0; i < N; ++i) { const int k = at + i; if (k >= 0 && k < len) y[k] += tout[(size_t) i].real() * win[(size_t) i] / 1.5f; }
            }
            if (cancel != nullptr && cancel->load()) return false;
        }
        if (progress) progress->store (0.75f + 0.06f * (float) (s + 1));
    }
    for (int ch = 0; ch < nch; ++ch)
    {
        float* m = out[StemMelodies].getWritePointer (ch);
        const float* x = chan (ch);
        for (int i = 0; i < len; ++i) m[i] = x[i] - out[StemDrums].getSample (ch, i) - out[StemBass].getSample (ch, i) - out[StemVocals].getSample (ch, i);
    }
    if (src.getNumChannels() == 1) for (auto& b : out) { b.addFrom (0, 0, b, 1, 0, len); b.applyGain (0.5f); b.setSize (1, len, true); }
    if (progress) progress->store (1.0f);
    return true;
}

std::array<juce::File, NumStems> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir,
                                                    std::atomic<float>* progress, std::atomic<bool>* cancel)
{
    std::array<juce::File, NumStems> files {};
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (in));
    if (r == nullptr || r->lengthInSamples <= 0) return files;
    const auto a = (juce::int64) juce::jmax (0.0, startSec * r->sampleRate);
    const auto n = (int) juce::jmin ((juce::int64) (lengthSec * r->sampleRate), r->lengthInSamples - a, (juce::int64) (r->sampleRate * 60 * 12));
    if (n <= 0) return files;
    juce::AudioBuffer<float> src ((int) juce::jmax (1u, juce::jmin (2u, r->numChannels)), n);
    r->read (&src, 0, n, a, true, true);
    std::array<juce::AudioBuffer<float>, NumStems> out;
    if (! splitStems (src, r->sampleRate, out, progress, cancel)) return files;
    outDir.createDirectory();
    for (int s = 0; s < NumStems; ++s)
    {
        auto f = outDir.getNonexistentChildFile (in.getFileNameWithoutExtension() + " - " + stemName (s), ".wav");
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), r->sampleRate, (unsigned) out[(size_t) s].getNumChannels(), 32, {}, 0));
        if (w == nullptr) return {};
        os.release();
        w->writeFromAudioSampleBuffer (out[(size_t) s], 0, n);
        files[(size_t) s] = f;
    }
    return files;
}
}
