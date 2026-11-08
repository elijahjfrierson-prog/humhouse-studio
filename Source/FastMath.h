#pragma once
#include <cstdint>
#include <cstring>

// Cheap, smooth replacements for libm calls in per-sample DSP (accurate well below audibility).
namespace hhs
{
// rational tanh: error < 1e-4, exact +-1 limits, NaN passes through
template <typename T> inline T fastTanh (T x) noexcept
{
    x = x < (T) -9 ? (T) -9 : (x > (T) 9 ? (T) 9 : x);
    const T x2 = x * x;
    const T y = x * ((T) 135135 + x2 * ((T) 17325 + x2 * ((T) 378 + x2)))
              / ((T) 135135 + x2 * ((T) 62370 + x2 * ((T) 3150 + (T) 28 * x2)));
    return y > (T) 1 ? (T) 1 : (y < (T) -1 ? (T) -1 : y);
}

// log2 for normal positive floats (callers clamp away 0): error ~5e-06
inline float fastLog2 (float x) noexcept
{
    std::uint32_t i; std::memcpy (&i, &x, 4);
    const float e = (float) ((int) ((i >> 23) & 255) - 127);
    i = (i & 0x007FFFFFu) | 0x3F800000u;
    float m; std::memcpy (&m, &i, 4);
    static constexpr float c[] { -2.482598444e-02f, 4.342890782e-02f, -7.068611582e-02f, 1.418487430e-01f, -3.206247845e-01f, 9.618147835e-01f, 5.849626827e-01f };
    const float t = m - 1.5f;
    float p = c[0];
    for (int k = 1; k < 7; ++k) p = p * t + c[k];
    return e + p;
}
inline float fastLog10 (float x) noexcept { return 0.30102999566f * fastLog2 (x); }
}
