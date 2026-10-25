#include "HumLab.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <complex>

namespace hhs::humlab
{
static void span (const juce::AudioBuffer<float>& b, int& a, int& e)
{
    const int n = b.getNumSamples();
    if (e <= a) { a = 0; e = n; }
    a = juce::jlimit (0, n, a); e = juce::jlimit (a, n, e);
}
static void edgeRamps (juce::AudioBuffer<float>& b, int a, int e)
{
    const int x = juce::jmin (32, (e - a) / 4);
    if (x < 2) return;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        if (a > 0) b.applyGainRamp (ch, a, x, 0.0f, 1.0f);
        if (e < b.getNumSamples()) b.applyGainRamp (ch, e - x, x, 1.0f, 0.0f);
    }
}

bool cut (juce::AudioBuffer<float>& b, int a, int e)
{
    span (b, a, e);
    const int n = b.getNumSamples(), keep = n - (e - a);
    if (keep < 64) return false;
    const int x = juce::jmin (64, a, n - e);
    juce::AudioBuffer<float> out (b.getNumChannels(), keep);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* s = b.getReadPointer (ch); float* d = out.getWritePointer (ch);
        std::copy (s, s + a, d);
        for (int i = 0; i < x; ++i) { const float t = (i + 0.5f) / (float) x; d[a - x + i] = s[a - x + i] * (1.0f - t) + s[e - x + i] * t; }
        std::copy (s + e, s + n, d + a);
    }
    b = std::move (out);
    return true;
}

bool trim (juce::AudioBuffer<float>& b, int a, int e)
{
    span (b, a, e);
    if (e - a < 64) return false;
    juce::AudioBuffer<float> out (b.getNumChannels(), e - a);
    for (int ch = 0; ch < b.getNumChannels(); ++ch) out.copyFrom (ch, 0, b, ch, a, e - a);
    const int x = juce::jmin (32, (e - a) / 4);
    for (int ch = 0; ch < out.getNumChannels(); ++ch) { out.applyGainRamp (ch, 0, x, 0.0f, 1.0f); out.applyGainRamp (ch, e - a - x, x, 1.0f, 0.0f); }
    b = std::move (out);
    return true;
}

void silence (juce::AudioBuffer<float>& b, int a, int e)
{
    span (b, a, e);
    const int x = juce::jmin (32, (e - a) / 4);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        if (a > 0 && x > 1) b.applyGainRamp (ch, a, x, 1.0f, 0.0f);
        b.clear (ch, a + (a > 0 ? x : 0), e - a - (a > 0 ? x : 0) - (e < b.getNumSamples() ? x : 0));
        if (e < b.getNumSamples() && x > 1) b.applyGainRamp (ch, e - x, x, 0.0f, 1.0f);
    }
}

void fade (juce::AudioBuffer<float>& b, int a, int e, bool fadeIn)
{
    span (b, a, e);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int i = a; i < e; ++i)
        {
            const float t = (i - a + 0.5f) / (float) juce::jmax (1, e - a);
            const float g = std::sin ((fadeIn ? t : 1.0f - t) * juce::MathConstants<float>::halfPi);   // equal-power curve
            d[i] *= g;
        }
    }
}

void normalize (juce::AudioBuffer<float>& b, int a, int e, float peakDb)
{
    span (b, a, e);
    float pk = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch) pk = juce::jmax (pk, b.getMagnitude (ch, a, e - a));
    if (pk < 1.0e-6f) return;
    b.applyGain (a, e - a, juce::Decibels::decibelsToGain (peakDb) / pk);
}

void reverse (juce::AudioBuffer<float>& b, int a, int e)
{
    span (b, a, e);
    for (int ch = 0; ch < b.getNumChannels(); ++ch) std::reverse (b.getWritePointer (ch) + a, b.getWritePointer (ch) + e);
    edgeRamps (b, a, e);
}

void gain (juce::AudioBuffer<float>& b, int a, int e, float db)
{
    span (b, a, e);
    b.applyGain (a, e - a, juce::Decibels::decibelsToGain (db));
}

void denoise (juce::AudioBuffer<float>& b, double, int a, int e, float amount)
{
    span (b, a, e);
    constexpr int order = 11, N = 1 << order, hop = N / 4;
    if (e - a < N) return;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
    const int len = e - a, frames = (len - N) / hop + 1, bins = N / 2 + 1;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* src = b.getReadPointer (ch) + a;
        std::vector<std::vector<std::complex<float>>> spec ((size_t) frames, std::vector<std::complex<float>> ((size_t) N));
        std::vector<float> energy ((size_t) frames);
        std::vector<std::complex<float>> in ((size_t) N);
        for (int f = 0; f < frames; ++f)
        {
            for (int i = 0; i < N; ++i) in[(size_t) i] = src[f * hop + i] * win[(size_t) i];
            fft.perform (in.data(), spec[(size_t) f].data(), false);
            double en = 0; for (int k = 0; k < bins; ++k) en += std::norm (spec[(size_t) f][(size_t) k]);
            energy[(size_t) f] = (float) en;
        }
        // noise print: average spectrum of the quietest 10% of frames
        std::vector<int> order_ ((size_t) frames);
        for (int f = 0; f < frames; ++f) order_[(size_t) f] = f;
        std::sort (order_.begin(), order_.end(), [&] (int x, int y) { return energy[(size_t) x] < energy[(size_t) y]; });
        const int q = juce::jmax (1, frames / 10);
        std::vector<float> noise ((size_t) bins, 0.0f);
        for (int j = 0; j < q; ++j) for (int k = 0; k < bins; ++k) noise[(size_t) k] += std::abs (spec[(size_t) order_[(size_t) j]][(size_t) k]) / q;
        std::vector<float> out ((size_t) len, 0.0f), wsum ((size_t) len, 0.0f), prevG ((size_t) bins, 1.0f);
        const float floorG = juce::Decibels::decibelsToGain (-24.0f * amount), over = 1.5f + 1.5f * amount;
        std::vector<std::complex<float>> back ((size_t) N);
        for (int f = 0; f < frames; ++f)
        {
            auto& S = spec[(size_t) f];
            for (int k = 0; k < bins; ++k)
            {
                const float mag = std::abs (S[(size_t) k]) + 1.0e-9f;
                float g = juce::jlimit (floorG, 1.0f, 1.0f - over * noise[(size_t) k] / mag);
                g = juce::jmax (g, prevG[(size_t) k] * 0.6f);   // gentle release: no musical-noise chirps
                prevG[(size_t) k] = g;
                S[(size_t) k] *= g;
                if (k > 0 && k < N / 2) S[(size_t) (N - k)] = std::conj (S[(size_t) k]);
            }
            fft.perform (S.data(), back.data(), true);
            for (int i = 0; i < N; ++i)
            {
                out[(size_t) (f * hop + i)] += back[(size_t) i].real() * win[(size_t) i];
                wsum[(size_t) (f * hop + i)] += win[(size_t) i] * win[(size_t) i];
            }
        }
        float* d = b.getWritePointer (ch) + a;
        for (int i = 0; i < len; ++i)
        {
            const float w = wsum[(size_t) i];
            if (w > 0.1f) d[i] = out[(size_t) i] / w;
            else if (w > 1.0e-4f) { const float t = w / 0.1f; d[i] = d[i] * (1.0f - t) * floorG + out[(size_t) i] / w * t; }
            else d[i] *= floorG;
        }
    }
}

std::vector<float> spectrum (const juce::AudioBuffer<float>& b, double sr, int a, int e, int bands)
{
    span (b, a, e);
    constexpr int order = 12, N = 1 << order;
    std::vector<float> outDb ((size_t) juce::jmax (1, bands), -120.0f);
    if (e - a < N || sr <= 0) return outDb;
    juce::dsp::FFT fft (order);
    std::vector<float> acc ((size_t) N / 2, 0.0f), w ((size_t) N * 2);
    const int frames = juce::jmin (64, (e - a - N) / (N / 2) + 1);
    const double step = frames > 1 ? (double) (e - a - N) / (frames - 1) : 0.0;
    for (int f = 0; f < frames; ++f)
    {
        const int at = a + (int) (f * step);
        std::fill (w.begin(), w.end(), 0.0f);
        for (int i = 0; i < N; ++i)
            w[(size_t) i] = 0.5f * (b.getSample (0, at + i) + b.getSample (b.getNumChannels() > 1 ? 1 : 0, at + i)) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N));
        fft.performFrequencyOnlyForwardTransform (w.data());
        for (int k = 0; k < N / 2; ++k) acc[(size_t) k] += w[(size_t) k] / frames;
    }
    for (int j = 0; j < bands; ++j)
    {
        const double f0 = 30.0 * std::pow (20000.0 / 30.0, j / (double) bands), f1 = 30.0 * std::pow (20000.0 / 30.0, (j + 1) / (double) bands);
        const int k0 = juce::jlimit (1, N / 2 - 1, (int) (f0 * N / sr)), k1 = juce::jlimit (k0 + 1, N / 2, (int) std::ceil (f1 * N / sr));
        float m = 0; for (int k = k0; k < k1; ++k) m = juce::jmax (m, acc[(size_t) k]);
        outDb[(size_t) j] = juce::Decibels::gainToDecibels (m * 4.0f / N, -120.0f);
    }
    return outDb;
}
}
