// Minimum-phase RBJ biquads (transposed direct form II). Every filter in the
// plug-in is built from these, so nothing here introduces group delay beyond
// the filter's own analog-equivalent phase response: 0 samples of latency.

#pragma once

#include <algorithm>
#include <cmath>

namespace rockglue
{

struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoefficients identity() { return {}; }

    // 2nd-order Butterworth when q = 1/sqrt(2).
    static BiquadCoefficients highPass(double fs, double freq, double q = 0.70710678118654752)
    {
        const double w0 = 2.0 * pi() * freq / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        BiquadCoefficients c;
        c.b0 = ((1.0 + cw) * 0.5) / a0;
        c.b1 = (-(1.0 + cw)) / a0;
        c.b2 = ((1.0 + cw) * 0.5) / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    static BiquadCoefficients lowPass(double fs, double freq, double q = 0.70710678118654752)
    {
        const double w0 = 2.0 * pi() * freq / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        BiquadCoefficients c;
        c.b0 = ((1.0 - cw) * 0.5) / a0;
        c.b1 = (1.0 - cw) / a0;
        c.b2 = ((1.0 - cw) * 0.5) / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    // Constant-Q parametric bell. gainDb may be negative (cut).
    static BiquadCoefficients peak(double fs, double freq, double q, double gainDb)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * pi() * freq / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha / A;
        BiquadCoefficients c;
        c.b0 = (1.0 + alpha * A) / a0;
        c.b1 = (-2.0 * cw) / a0;
        c.b2 = (1.0 - alpha * A) / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha / A) / a0;
        return c;
    }

    static constexpr double pi() { return 3.14159265358979323846; }
};

class Biquad
{
public:
    void setCoefficients(const BiquadCoefficients& c) { coeffs = c; }
    void reset() { z1 = z2 = 0.0; }

    inline float process(float x) noexcept
    {
        const double in = static_cast<double>(x);
        const double out = coeffs.b0 * in + z1;
        z1 = coeffs.b1 * in - coeffs.a1 * out + z2;
        z2 = coeffs.b2 * in - coeffs.a2 * out;
        return static_cast<float>(out);
    }

private:
    BiquadCoefficients coeffs;
    double z1 = 0.0, z2 = 0.0;
};

// Stereo pair sharing one coefficient set.
class StereoBiquad
{
public:
    void setCoefficients(const BiquadCoefficients& c) { left.setCoefficients(c); right.setCoefficients(c); }
    void reset() { left.reset(); right.reset(); }
    inline void process(float& l, float& r) noexcept { l = left.process(l); r = right.process(r); }

private:
    Biquad left, right;
};

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) noexcept { return 20.0f * std::log10(std::max(g, 1.0e-9f)); }

// One-pole coefficient for a time constant in milliseconds.
inline float timeConstantCoefficient(double ms, double fs) noexcept
{
    if (ms <= 0.0)
        return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * fs)));
}

} // namespace rockglue
