#include "UI.h"
#include "Drummer/DrummerCore.h"

namespace hhs
{

struct PoolInstrument
{
    const char* name; juce::uint32 colour; int wave; float a, d, s, r, cut, res, det; int uni; float drv, fmr, fmi, mor, noi, lfo, vib, glide; bool mono; int root;
    int table = 0; float pos = 0;                                       // HumWave (wave 10): factory table + position
    std::array<int, 3> xw { 0, 1, 3 };                                   // 3x Oscillation (wave 9): shapes, levels, coarse, fine
    std::array<float, 3> xv { 1, 0, 0 }, xs { 0, 0, 0 }, xf { 0, 0, 0 };
    int warp = 0; float warpAmt = 0, blend = 0.5f, sub = 0;              // HumWave Serum-style warp, unison blend, sub osc
    const char* cat = nullptr;                                           // HumWave Serum Pack folder (nullptr = main list)
};
// HumWave Serum Pack: wavetable + warp presets. The Weird FX folder is voiced after the Triple S Hitters one-shots
// (measured: ~6.5 kHz brightness, very noisy/gritty spectrum, ~3.7 s tails)
static PoolInstrument wp (const char* cat, const char* name, juce::uint32 colour, int table, float pos, int warp, float warpAmt,
                          int uni, float det, float blend, float a, float d, float s, float r, float cut, float res,
                          float drv, float noi, float sub, float lfo, float mor, float glide = 0, bool mono = false, int root = 60, float fmr = 2)
{
    PoolInstrument e { name, colour, 10, a, d, s, r, cut, res, det, uni, drv, fmr, 0, mor, noi, lfo, 0, glide, mono, root };
    e.table = table; e.pos = pos; e.warp = warp; e.warpAmt = warpAmt; e.blend = blend; e.sub = sub; e.cat = cat;
    return e;
}
// Sonic Store: our own instrument library (pianos, keys, organs, guitars, basses, synths, pads, strings, brass, bells, FX)
static PoolInstrument ss (const char* cat, const char* name, juce::uint32 colour, int wave, float a, float d, float s, float r, float cut, float res,
                          float det, int uni, float drv, float fmr, float fmi, float mor, float noi, float lfo, float vib, float glide = 0, bool mono = false, int root = 60)
{
    PoolInstrument e { name, colour, wave, a, d, s, r, cut, res, det, uni, drv, fmr, fmi, mor, noi, lfo, vib, glide, mono, root };
    e.cat = cat;
    return e;
}
// drawbar organ on 3x Oscillation: three sine drawbars (coarse in semitones) + rotary wobble (LFO vibrato)
static PoolInstrument organ (const char* name, juce::uint32 colour, std::array<float, 3> bars, std::array<float, 3> semis, float a, float r, float cut,
                             float drv, float noi, float lfo, float vib, float decay = 0.4f, float sustain = 1.0f)
{
    PoolInstrument e { name, colour, 9, a, decay, sustain, r, cut, 0.05f, 0, 1, drv, 1, 0, 0, noi, lfo, vib, 0, false, 60 };
    e.xw = { 3, 3, 3 }; e.xv = bars; e.xs = semis; e.cat = "SS:Organs";
    return e;
}
static std::vector<PoolInstrument> withSonicStore (std::vector<PoolInstrument> v)
{
    const char *PI = "SS:Pianos", *KY = "SS:Keys", *GT = "SS:Guitars", *BS = "SS:Basses", *SY = "SS:Synths", *PD = "SS:Pads",
               *ST = "SS:Strings & Brass", *BL = "SS:Bells & Mallets", *FX = "SS:FX";
    const std::vector<PoolInstrument> store {
        //  folder name                      colour      wv a       d     s     r     cut    res   det  uni drv   fmr  fmi  mor  noi   lfo   vib
        organ ("SSS Organ",                  0xffff9a3d, { 1.0f, 0.8f, 0.6f }, { 0, 12, 19 }, 0.003f, 0.06f, 9000, 0.25f, 0.02f, 6.5f, 0.06f, 0.35f, 0.95f),
        organ ("SSS Organ Slow Rotor",       0xffffb05a, { 1.0f, 0.8f, 0.6f }, { 0, 12, 19 }, 0.003f, 0.08f, 8000, 0.2f,  0.02f, 0.8f, 0.08f),
        organ ("Gospel Organ",               0xffff7a3d, { 1.0f, 0.9f, 0.7f }, { -12, 0, 12 }, 0.002f, 0.1f, 10000, 0.3f, 0.02f, 6.0f, 0.07f),
        organ ("Jazz Organ",                 0xffe8a050, { 1.0f, 0.7f, 0.0f }, { 0, 19, 0 }, 0.002f, 0.05f, 7000, 0.15f, 0.015f, 5.5f, 0.05f, 0.25f, 0.85f),
        organ ("Rock Organ",                 0xffff5a2a, { 1.0f, 1.0f, 0.8f }, { 0, 12, 24 }, 0.002f, 0.06f, 11000, 0.6f, 0.02f, 6.8f, 0.07f),
        organ ("Church Organ",               0xffd8c080, { 1.0f, 0.7f, 0.5f }, { -12, 0, 7 }, 0.08f, 0.6f, 6000, 0.0f, 0.01f, 0, 0),
        organ ("Combo Organ",                0xffffd04d, { 0.9f, 0.9f, 0.4f }, { 0, 12, 31 }, 0.002f, 0.05f, 12000, 0.35f, 0.02f, 7.0f, 0.04f),
        ss (PI, "Grand Piano",               0xfff2f2f2, 6, 0.001f,3.0f, 0,    0.5f, 9500,  0.05f,0,   1, 0,    1,   2.6f,0,   0,    0,    0),
        ss (PI, "Bright Piano",              0xffffffff, 6, 0.001f,2.6f, 0,    0.45f,13000, 0.05f,0,   1, 0.05f,1,   3.4f,0,   0,    0,    0),
        ss (PI, "Felt Piano",                0xffd8cfc0, 6, 0.004f,2.2f, 0,    0.6f, 3200,  0.05f,0,   1, 0,    1,   1.6f,0,   0.01f,0,    0),
        ss (PI, "Honky Tonk",                0xffe8d8a0, 6, 0.001f,2.4f, 0,    0.45f,8000,  0.05f,0.3f,2, 0.05f,1,   2.8f,0,   0,    0,    0),
        ss (PI, "Lo-Fi Upright",             0xffc8b890, 6, 0.002f,2.0f, 0,    0.4f, 4200,  0.1f, 0.1f,1, 0.15f,1,   2.0f,0,   0.04f,0.5f, 0.06f),
        ss (KY, "Suitcase EP",               0xffff8a5a, 6, 0.002f,2.5f, 0.3f, 0.6f, 5500,  0.1f, 0,   1, 0.12f,1,   1.3f,0,   0,    5.0f, 0.03f),
        ss (KY, "Wurly",                     0xffffa06a, 6, 0.002f,1.8f, 0.25f,0.5f, 4800,  0.15f,0,   1, 0.25f,1,   2.2f,0,   0,    6.0f, 0.04f),
        ss (KY, "DX Tine",                   0xffffc0a0, 6, 0.001f,2.2f, 0.2f, 0.6f, 11000, 0.05f,0,   1, 0,    14,  1.6f,0,   0,    0,    0),
        ss (KY, "Clav",                      0xffd0ff8a, 1, 0.001f,0.5f, 0.2f, 0.12f,3800,  0.4f, 0,   1, 0.2f, 1,   0,   0,   0,    0,    0),
        ss (KY, "Harpsichord",               0xfff0e0b0, 0, 0.001f,1.2f, 0,    0.3f, 9000,  0.15f,0.08f,2,0,    1,   0,   0,   0,    0,    0),
        ss (GT, "Nylon Guitar",              0xffd9a066, 2, 0.001f,1.4f, 0,    0.35f,2800,  0.25f,0,   1, 0.05f,1,   0,   0,   0.01f,0,    0),
        ss (GT, "Clean Electric",            0xffc8a070, 0, 0.001f,1.6f, 0.15f,0.4f, 3600,  0.3f, 0.1f,2, 0.1f, 1,   0,   0,   0,    4.5f, 0.02f),
        ss (GT, "Crunch Guitar",             0xffd06a3d, 0, 0.002f,1.2f, 0.5f, 0.3f, 3000,  0.3f, 0.15f,2,0.6f, 1,   0,   0,   0,    0,    0),
        ss (GT, "Lead Guitar",               0xffff6a3d, 0, 0.003f,1.0f, 0.8f, 0.4f, 4200,  0.35f,0.1f,2, 0.8f, 1,   0,   0,   0,    5.5f, 0.12f,0.04f,true),
        ss (GT, "Koto Pluck",                0xfff0c080, 6, 0.001f,0.9f, 0,    0.3f, 7000,  0.1f, 0,   1, 0,    3,   1.2f,0,   0,    0,    0,    0,    false, 72),
        ss (BS, "Finger Bass",               0xff9a6a3d, 2, 0.003f,0.9f, 0.4f, 0.15f,1500,  0.25f,0,   1, 0.2f, 1,   0,   0,   0,    0,    0,    0.03f,true,  36),
        ss (BS, "Slap Bass",                 0xffb07a4a, 6, 0.001f,0.6f, 0.3f, 0.15f,3500,  0.2f, 0,   1, 0.2f, 2,   2.5f,0,   0,    0,    0,    0.02f,true,  36),
        ss (BS, "Synth Bass 101",            0xffff4d4d, 1, 0.002f,0.4f, 0.4f, 0.1f, 1200,  0.6f, 0,   1, 0.3f, 1,   0,   0,   0,    0,    0,    0.05f,true,  36),
        ss (BS, "Upright Bass",              0xff8a5a3d, 3, 0.004f,1.2f, 0.2f, 0.2f, 900,   0.15f,0,   1, 0.25f,1,   0,   0,   0.02f,0,    0,    0,    true,  36),
        ss (BS, "Moog Bass",                 0xffd04a2a, 0, 0.002f,0.5f, 0.5f, 0.12f,800,   0.7f, 0.15f,2,0.4f, 1,   0,   0,   0,    0,    0,    0.06f,true,  36),
        ss (SY, "Poly Brass Synth",          0xffffb04d, 0, 0.04f, 0.5f, 0.7f, 0.3f, 5000,  0.3f, 0.3f,2, 0.1f, 1,   0,   0,   0,    0,    0),
        ss (SY, "Jupiter Saw",               0xff5ad1ff, 5, 0.01f, 0.5f, 0.8f, 0.4f, 8000,  0.25f,0.45f,1,0.05f,1,   0,   0,   0,    0,    0),
        ss (SY, "Square Lead",               0xff8aff6a, 1, 0.003f,0.4f, 0.8f, 0.2f, 7000,  0.2f, 0.1f,2, 0.1f, 1,   0,   0,   0,    5.5f, 0.1f, 0.04f,true),
        ss (SY, "Sync Arp",                  0xff7dff8a, 0, 0.001f,0.25f,0,    0.15f,9000,  0.45f,0,   1, 0.2f, 1,   0,   0,   0,    0,    0),
        ss (SY, "Hoover",                    0xffb06bff, 5, 0.01f, 0.5f, 0.9f, 0.4f, 6000,  0.3f, 0.8f,1, 0.3f, 1,   0,   0,   0,    6.0f, 0.3f, 0.1f, true),
        ss (PD, "Warm Pad",                  0xffb06bff, 7, 0.9f,  1.0f, 0.85f,1.8f, 4500,  0.2f, 0,   1, 0,    1,   0,   0.25f,0,   0.12f,0.03f),
        ss (PD, "Analog Pad",                0xff9a8aff, 5, 0.7f,  1.0f, 0.9f, 1.6f, 3800,  0.2f, 0.35f,1,0,    1,   0,   0,   0,    0.2f, 0.04f),
        ss (PD, "Air Pad",                   0xffc0e0ff, 8, 1.2f,  1.5f, 0.9f, 2.5f, 7000,  0.2f, 0,   1, 0,    1,   0,   0,   0.4f, 0.1f, 0.02f),
        ss (PD, "Dark Pad",                  0xff4a4aa0, 7, 1.5f,  2.0f, 0.9f, 3.0f, 1800,  0.4f, 0,   1, 0.1f, 1,   0,   0.6f,0.05f,0.08f,0.02f),
        ss (ST, "String Ensemble",           0xffc9a0ff, 5, 0.4f,  1.0f, 0.9f, 0.9f, 5000,  0.15f,0.35f,1,0,    1,   0,   0,   0,    5.0f, 0.08f),
        ss (ST, "Pizzicato",                 0xffe0c0ff, 2, 0.001f,0.35f,0,    0.2f, 4000,  0.2f, 0.1f,2, 0,    1,   0,   0,   0,    0,    0),
        ss (ST, "Cello",                     0xffa080d0, 0, 0.15f, 1.0f, 0.85f,0.5f, 2400,  0.2f, 0.1f,2, 0.05f,1,   0,   0,   0,    5.0f, 0.1f, 0,    false, 48),
        ss (ST, "Brass Section",             0xffffc04d, 0, 0.06f, 0.6f, 0.8f, 0.25f,3600,  0.25f,0.25f,2,0.2f, 1,   0,   0,   0,    5.0f, 0.06f),
        ss (ST, "Trumpet",                   0xffffd070, 1, 0.04f, 0.6f, 0.8f, 0.2f, 4200,  0.2f, 0,   1, 0.25f,1,   0,   0,   0.01f,5.5f, 0.1f, 0.03f,true),
        ss (ST, "Flute",                     0xffe0fff0, 3, 0.06f, 0.5f, 0.9f, 0.2f, 6000,  0.1f, 0,   1, 0,    1,   0,   0,   0.12f,5.0f, 0.08f,0.02f,true,  72),
        ss (BL, "Vibraphone",                0xffc8e8ff, 6, 0.001f,2.5f, 0,    1.5f, 9000,  0.05f,0,   1, 0,    4,   1.2f,0,   0,    5.0f, 0.02f,0,    false, 72),
        ss (BL, "Marimba",                   0xffd8a070, 6, 0.001f,0.6f, 0,    0.3f, 6000,  0.05f,0,   1, 0,    4,   1.8f,0,   0,    0,    0,    0,    false, 60),
        ss (BL, "Celesta",                   0xfff0f0ff, 6, 0.001f,1.6f, 0,    0.8f, 14000, 0.05f,0,   1, 0,    7,   1.4f,0,   0,    0,    0,    0,    false, 84),
        ss (BL, "Tubular Bell",              0xffe8e8c0, 6, 0.001f,3.5f, 0,    2.5f, 12000, 0.05f,0,   1, 0,    3.5f,4.5f,0,   0,    0,    0),
        ss (BL, "Steel Drum",                0xffffe0a0, 6, 0.001f,1.0f, 0,    0.5f, 9000,  0.1f, 0,   1, 0,    2.5f,2.0f,0,   0,    0,    0,    0,    false, 72),
        ss (FX, "Riser Sweep",               0xffff8aff, 8, 3.0f,  1.0f, 1.0f, 0.5f, 12000, 0.5f, 0,   1, 0.2f, 1,   0,   0,   0.8f, 0.2f, 0.5f),
        ss (FX, "Sci-Fi Zap",                0xff3dffd0, 6, 0.001f,0.3f, 0,    0.2f, 15000, 0.4f, 0,   1, 0.3f, 7.5f,8,   0,   0,    9.0f, 1.0f),
        ss (FX, "Wind",                      0xffa0c0d0, 8, 1.5f,  2.0f, 1.0f, 2.0f, 3000,  0.6f, 0,   1, 0,    1,   0,   0,   1.0f, 0.15f,0),
        ss (FX, "Noise Hit",                 0xffd0d0d0, 8, 0.001f,0.8f, 0,    0.6f, 8000,  0.3f, 0,   1, 0.4f, 1,   0,   0,   1.0f, 0,    0),
    };
    v.insert (v.end(), store.begin(), store.end());
    return v;
}
static std::vector<PoolInstrument> withSerumPack (std::vector<PoolInstrument> v)
{
    v = withSonicStore (std::move (v));
    const char *B = "Bass", *L = "Leads", *P = "Plucks", *D = "Pads", *K = "Keys & Bells", *W = "Weird FX";
    const std::vector<PoolInstrument> pack {
        //  folder name                    colour      tbl pos   warp amt  uni det   bld  a      d     s     r     cut    res   drv   noi   sub   lfo   mor  glide  mono  root
        wp (B, "Reese Grinder",            0xffff3d6a, 0,  0.9f, 2,  0.4f, 5, 0.35f,0.7f,0.003f,0.5f, 0.9f, 0.2f, 1800,  0.35f,0.45f,0,    0.5f, 0.3f, 0.2f, 0.06f, true,  36),
        wp (B, "Growl Talker",             0xffff6a3d, 2,  0.3f, 9,  0.6f, 3, 0.2f, 0.5f,0.002f,0.4f, 0.8f, 0.15f,3500,  0.45f,0.5f, 0,    0.4f, 3.0f, 0.6f, 0.05f, true,  36, 3),
        wp (B, "Sync Snarl",               0xffff8a3d, 5,  0.5f, 1,  0.55f,1, 0,    0.5f,0.002f,0.35f,0.7f, 0.12f,4200,  0.3f, 0.4f, 0,    0.5f, 0.5f, 0.3f, 0.04f, true,  36),
        wp (B, "Folded Sub",               0xffd94a4a, 3,  0.15f,11, 0.3f, 1, 0,    0.5f,0.002f,0.9f, 0.6f, 0.2f, 900,   0.2f, 0.2f, 0,    0.7f, 0,    0,    0.07f, true,  36),
        wp (B, "Neuro Wobble",             0xffb03dff, 11, 0.4f, 3,  0.5f, 3, 0.25f,0.6f,0.003f,0.6f, 0.9f, 0.18f,2600,  0.55f,0.5f, 0,    0.4f, 4.0f, 0.8f, 0.04f, true,  36),
        wp (B, "Digital 808",              0xffff4d4d, 4,  0.1f, 8,  0.3f, 1, 0,    0.5f,0.001f,1.4f, 0.4f, 0.3f, 1200,  0.15f,0.35f,0,    0.8f, 0,    0,    0.08f, true,  36),
        wp (B, "Rubber Bounce",            0xffff7ab0, 10, 0.35f,10, 0.6f, 1, 0,    0.5f,0.002f,0.3f, 0.5f, 0.1f, 3000,  0.4f, 0.25f,0,    0.5f, 0,    0,    0.05f, true,  36),
        wp (B, "Mirror Bass",              0xffff5a8a, 0,  0.6f, 6,  0.7f, 2, 0.15f,0.5f,0.002f,0.5f, 0.8f, 0.15f,2400,  0.3f, 0.3f, 0,    0.6f, 0,    0,    0.05f, true,  36),
        wp (B, "PWM Underground",          0xffc94a2a, 1,  0.4f, 4,  0.45f,3, 0.2f, 0.6f,0.003f,0.6f, 0.85f,0.2f, 1600,  0.3f, 0.3f, 0,    0.5f, 0.3f, 0.4f, 0.05f, true,  36),
        wp (B, "Bit Rot Bass",             0xffa03030, 9,  0.5f, 12, 0.4f, 1, 0,    0.5f,0.002f,0.5f, 0.7f, 0.12f,5000,  0.2f, 0.5f, 0,    0.5f, 0,    0,    0.05f, true,  36),
        wp (B, "Hollow Square Sub",        0xffd06a6a, 1,  0.2f, 5,  0.4f, 1, 0,    0.5f,0.002f,1.0f, 0.7f, 0.25f,1100,  0.2f, 0.15f,0,    0.6f, 0,    0,    0.08f, true,  36),
        wp (L, "Serum Supersaw",           0xff5ad1ff, 0,  1.0f, 0,  0,    7, 0.45f,0.8f,0.01f, 0.4f, 0.85f,0.35f,12000, 0.2f, 0.1f, 0,    0,    5.5f, 0,    0,     false, 60),
        wp (L, "Bend Lead",                0xff38d1ff, 0,  0.8f, 2,  0.6f, 3, 0.2f, 0.5f,0.005f,0.4f, 0.8f, 0.3f, 9000,  0.25f,0.2f, 0,    0,    0,    0,    0.04f, true,  60),
        wp (L, "Vowel Screamer",           0xffffb0e0, 2,  0.6f, 9,  0.4f, 3, 0.2f, 0.5f,0.01f, 0.4f, 0.8f, 0.3f, 10000, 0.3f, 0.35f,0,    0,    5.0f, 0.4f, 0.05f, true,  60, 3),
        wp (L, "Hyper Sync Lead",          0xff7dffff, 5,  0.7f, 1,  0.6f, 5, 0.3f, 0.6f,0.005f,0.3f, 0.8f, 0.25f,11000, 0.2f, 0.3f, 0,    0,    0.25f,0.5f, 0.03f, true,  60),
        wp (L, "Flip Glide",               0xff9affb0, 6,  0.5f, 7,  0.5f, 3, 0.15f,0.5f,0.005f,0.3f, 0.85f,0.25f,9000,  0.2f, 0.15f,0,    0,    0,    0,    0.12f, true,  60),
        wp (L, "Alien Whistle",            0xff8aff6a, 8,  0.5f, 10, 0.5f, 1, 0,    0.5f,0.02f, 0.4f, 0.9f, 0.4f, 14000, 0.15f,0,    0,    0,    6.0f, 0.5f, 0.08f, true,  72),
        wp (L, "Rage Lead",                0xffff4d9a, 3,  0.6f, 11, 0.4f, 5, 0.35f,0.7f,0.003f,0.3f, 0.9f, 0.3f, 8000,  0.3f, 0.6f, 0,    0,    0,    0,    0.04f, true,  60),
        wp (L, "Detroit Saw",              0xff4dc3ff, 0,  0.95f,5,  0.3f, 7, 0.25f,0.6f,0.005f,0.3f, 0.8f, 0.3f, 7000,  0.25f,0.2f, 0,    0,    0,    0,    0,     false, 60),
        wp (L, "Quantized Chip",           0xffb0ff4d, 4,  0.7f, 8,  0.6f, 1, 0,    0.5f,0.001f,0.2f, 0.7f, 0.1f, 16000, 0.1f, 0,    0,    0,    0,    0,    0.02f, true,  72),
        wp (L, "Asym Siren",               0xffffe04d, 1,  0.5f, 5,  0.7f, 3, 0.2f, 0.5f,0.02f, 0.4f, 0.9f, 0.4f, 9000,  0.3f, 0.2f, 0,    0,    0.5f, 0.6f, 0.15f, true,  72),
        wp (L, "Organ Smear",              0xffffc06a, 6,  0.7f, 3,  0.3f, 3, 0.15f,0.4f,0.01f, 0.3f, 0.9f, 0.3f, 8000,  0.1f, 0.1f, 0,    0,    5.0f, 0.2f, 0,     false, 60),
        wp (P, "Glass Mirror Pluck",       0xff8affe0, 6,  0.6f, 6,  0.6f, 1, 0,    0.5f,0.001f,0.45f,0,    0.4f, 14000, 0.1f, 0,    0,    0,    0,    0,    0,     false, 72),
        wp (P, "Sync Pluck",               0xff6affc8, 5,  0.4f, 1,  0.4f, 3, 0.15f,0.4f,0.001f,0.3f, 0,    0.3f, 12000, 0.15f,0.1f, 0,    0,    0,    0,    0,     false, 72),
        wp (P, "Bell Bender",              0xffe8e8ff, 7,  0.5f, 2,  0.5f, 1, 0,    0.5f,0.001f,0.9f, 0,    0.8f, 15000, 0.1f, 0,    0,    0,    0,    0,    0,     false, 72),
        wp (P, "Digital Rain",             0xff7dff8a, 4,  0.5f, 12, 0.3f, 1, 0,    0.5f,0.001f,0.25f,0,    0.25f,13000, 0.15f,0,    0.05f,0,    0,    0,    0,     false, 72),
        wp (P, "Vowel Pop",                0xffffb0ff, 2,  0.4f, 9,  0.3f, 1, 0,    0.5f,0.001f,0.3f, 0,    0.2f, 9000,  0.2f, 0.1f, 0,    0,    0,    0,    0,     false, 72, 2),
        wp (P, "Plugg Pluck",              0xff6ac8ff, 0,  0.4f, 3,  0.3f, 3, 0.2f, 0.5f,0.001f,0.4f, 0,    0.35f,7000,  0.2f, 0,    0,    0,    0,    0,    0,     false, 72),
        wp (P, "Rubber Drop",              0xffff9aea, 10, 0.5f, 10, 0.5f, 1, 0,    0.5f,0.001f,0.35f,0,    0.3f, 10000, 0.25f,0.1f, 0,    0.2f, 0,    0,    0,     false, 60),
        wp (P, "Flip Mallet",              0xffc8ffe0, 3,  0.3f, 7,  0.7f, 1, 0,    0.5f,0.001f,0.5f, 0,    0.5f, 11000, 0.1f, 0,    0,    0,    0,    0,    0,     false, 72),
        wp (D, "Wavetable Dream",          0xffb06bff, 0,  0.5f, 2,  0.3f, 7, 0.3f, 0.7f,0.9f,  1.0f, 0.9f, 2.0f, 6000,  0.2f, 0,    0.05f,0,    0.12f,0.8f, 0,     false, 60),
        wp (D, "Vowel Choir",              0xffd0a0ff, 2,  0.4f, 6,  0.4f, 5, 0.25f,0.6f,0.8f,  1.0f, 0.9f, 1.8f, 7000,  0.15f,0,    0.04f,0,    0.2f, 0.7f, 0,     false, 60),
        wp (D, "PWM Strings",              0xffc9a0ff, 1,  0.5f, 4,  0.3f, 7, 0.35f,0.7f,0.6f,  1.0f, 0.9f, 1.4f, 5000,  0.15f,0,    0,    0,    0.3f, 0.6f, 0,     false, 60),
        wp (D, "Alien Atmos",              0xff6a7dff, 8,  0.5f, 10, 0.4f, 5, 0.3f, 0.6f,1.5f,  2.0f, 1.0f, 3.0f, 5000,  0.3f, 0,    0.15f,0,    0.08f,0.9f, 0,     false, 48),
        wp (D, "Organ Cloud",              0xffa0c0ff, 6,  0.6f, 0,  0,    7, 0.3f, 0.8f,0.7f,  1.0f, 0.9f, 2.0f, 4500,  0.1f, 0,    0.03f,0.2f, 0.15f,0.5f, 0,     false, 60),
        wp (D, "Frozen Fold",              0xff8ad8ff, 3,  0.3f, 11, 0.2f, 5, 0.2f, 0.6f,1.2f,  1.5f, 1.0f, 2.5f, 3500,  0.3f, 0,    0.1f, 0,    0.1f, 0.7f, 0,     false, 48),
        wp (D, "Metal Haze",               0xffc0c0e8, 7,  0.4f, 2,  0.3f, 5, 0.25f,0.6f,1.0f,  1.5f, 0.9f, 2.4f, 6000,  0.2f, 0,    0.12f,0,    0.06f,0.9f, 0,     false, 60),
        wp (D, "Sync Sunrise",             0xffffd08a, 5,  0.2f, 1,  0.2f, 7, 0.3f, 0.7f,1.4f,  1.0f, 1.0f, 2.0f, 8000,  0.15f,0,    0,    0,    0.05f,1.0f, 0,     false, 60),
        wp (K, "Glass Rhodes",             0xffff8a5a, 6,  0.3f, 9,  0.25f,1, 0,    0.5f,0.002f,2.2f, 0.25f,0.6f, 6000,  0.1f, 0.1f, 0,    0,    5.0f, 0.05f,0,     false, 60, 1),
        wp (K, "FM Kalimba",               0xfff2d28a, 7,  0.3f, 9,  0.4f, 1, 0,    0.5f,0.001f,0.8f, 0,    0.6f, 12000, 0.1f, 0,    0,    0,    0,    0,    0,     false, 72, 3),
        wp (K, "Toy Box",                  0xffffe0f0, 4,  0.6f, 8,  0.5f, 1, 0,    0.5f,0.001f,0.7f, 0,    0.5f, 14000, 0.1f, 0,    0,    0,    0,    0,    0,     false, 84),
        wp (K, "Bent Music Box",           0xffe0d0ff, 7,  0.6f, 3,  0.5f, 1, 0,    0.5f,0.001f,1.5f, 0,    1.2f, 15000, 0.05f,0,    0,    0,    3.0f, 0.1f, 0,     false, 84),
        wp (K, "Organ Keys",               0xffffb35a, 6,  0.8f, 0,  0,    1, 0,    0.5f,0.005f,0.5f, 0.9f, 0.2f, 7000,  0.1f, 0.15f,0,    0.2f, 5.5f, 0.05f,0,     false, 60),
        wp (K, "Mirror Bell",              0xffd8f0ff, 0,  0.3f, 6,  0.9f, 1, 0,    0.5f,0.001f,1.8f, 0,    1.4f, 16000, 0.05f,0,    0,    0,    0,    0,    0,     false, 72),
        wp (W, "Chaos Hitter",             0xffff3db0, 11, 0.6f, 12, 0.5f, 3, 0.4f, 0.6f,0.001f,3.7f, 0,    1.0f, 18000, 0.3f, 0.5f, 0.35f,0,    7.0f, 0.9f, 0,     false, 60),
        wp (W, "Static Angel",             0xffffffff, 8,  0.7f, 1,  0.8f, 5, 0.5f, 0.6f,0.01f, 3.5f, 0,    1.2f, 20000, 0.2f, 0.3f, 0.45f,0,    0.5f, 0.8f, 0,     false, 72),
        wp (W, "Glitch Gremlin",           0xff9aff3d, 9,  0.5f, 12, 0.8f, 1, 0,    0.5f,0.001f,1.2f, 0,    0.6f, 16000, 0.4f, 0.6f, 0.2f, 0,    11.0f,1.0f, 0,     false, 60),
        wp (W, "Robot Throat",             0xff3dffd0, 2,  0.5f, 9,  0.9f, 1, 0,    0.5f,0.002f,2.0f, 0.4f, 0.8f, 9000,  0.5f, 0.6f, 0.2f, 0.2f, 9.0f, 0.7f, 0.1f,  true,  48, 5),
        wp (W, "Cyber Goth Stab",          0xffd03dff, 11, 0.8f, 7,  0.6f, 5, 0.5f, 0.7f,0.001f,1.5f, 0,    0.8f, 17000, 0.3f, 0.7f, 0.3f, 0.3f, 0,    0,    0,     false, 60),
        wp (W, "Laser Rain",               0xff3dd0ff, 5,  1.0f, 1,  1.0f, 3, 0.6f, 0.5f,0.001f,0.6f, 0,    0.4f, 20000, 0.2f, 0.2f, 0.15f,0,    0,    0,    0.3f,  true,  84),
        wp (W, "Broken Radio",             0xffc8c870, 9,  0.3f, 8,  0.9f, 1, 0,    0.5f,0.02f, 3.7f, 0.3f, 1.5f, 6000,  0.5f, 0.5f, 0.5f, 0,    6.0f, 1.0f, 0,     false, 60),
        wp (W, "Wormhole",                 0xff6a3dff, 10, 0.5f, 10, 1.0f, 7, 0.6f, 0.8f,0.5f,  4.0f, 0.8f, 3.5f, 12000, 0.4f, 0.3f, 0.3f, 0,    0.3f, 1.0f, 0.5f,  true,  48),
        wp (W, "Demon Choir",              0xffff2020, 2,  0.2f, 11, 0.7f, 7, 0.4f, 0.7f,0.3f,  3.7f, 0.7f, 2.5f, 7000,  0.4f, 0.6f, 0.25f,0.3f, 0.2f, 0.9f, 0,     false, 48),
        wp (W, "Bit Crushed Angel",        0xffffd0ff, 4,  0.9f, 8,  0.8f, 5, 0.3f, 0.6f,0.02f, 3.0f, 0,    1.2f, 19000, 0.15f,0.3f, 0.4f, 0,    4.0f, 0.5f, 0,     false, 72),
        wp (W, "Metal Shredder",           0xffb0b0b0, 7,  1.0f, 9,  1.0f, 3, 0.5f, 0.6f,0.001f,1.8f, 0,    0.7f, 18000, 0.3f, 0.8f, 0.3f, 0,    0,    0,    0,     false, 60, 7),
        wp (W, "Spiral Siren",             0xffff8aff, 8,  0.5f, 6,  1.0f, 3, 0.4f, 0.6f,0.05f, 3.7f, 0.6f, 1.5f, 15000, 0.3f, 0.3f, 0.2f, 0,    2.0f, 1.0f, 0.6f,  true,  72),
        wp (W, "Aura Swell",               0xff8ad0ff, 11, 0.3f, 2,  0.7f, 7, 0.5f, 0.8f,2.0f,  3.7f, 0.9f, 3.0f, 16000, 0.2f, 0.2f, 0.4f, 0,    0.1f, 1.0f, 0,     false, 60),
        wp (W, "Glass Shatter",            0xffe0ffff, 11, 1.0f, 12, 0.9f, 3, 0.7f, 0.6f,0.001f,0.9f, 0,    0.9f, 20000, 0.1f, 0.4f, 0.6f, 0,    0,    0,    0,     false, 84),
        wp (W, "Rubber Ghost",             0xffd0ffd0, 10, 0.8f, 4,  0.8f, 5, 0.4f, 0.7f,0.4f,  3.0f, 0.5f, 2.0f, 9000,  0.35f,0.3f, 0.2f, 0,    3.0f, 0.8f, 0.2f,  true,  60),
        wp (W, "Hell Riser",               0xffff4000, 5,  0.0f, 1,  0.9f, 7, 0.6f, 0.7f,3.0f,  4.0f, 1.0f, 1.0f, 14000, 0.3f, 0.6f, 0.3f, 0.3f, 0.06f,1.0f, 0,     false, 48),
    };
    v.insert (v.end(), pack.begin(), pack.end());
    return v;
}
static void addSerumPackMenu (juce::PopupMenu& pool, const std::vector<PoolInstrument>& all)
{
    juce::PopupMenu pack;
    for (auto* folder : { "Bass", "Leads", "Plucks", "Pads", "Keys & Bells", "Weird FX" })
    {
        juce::PopupMenu sub;
        for (int i = 0; i < (int) all.size(); ++i)
            if (all[(size_t) i].cat != nullptr && juce::String (all[(size_t) i].cat) == folder) sub.addItem (2000 + i, all[(size_t) i].name);
        pack.addSubMenu (folder, sub);
    }
    pool.addSeparator();
    pool.addSubMenu ("HumWave Serum Pack", pack);
    juce::PopupMenu store;
    for (auto* folder : { "Pianos", "Keys", "Organs", "Guitars", "Basses", "Synths", "Pads", "Strings & Brass", "Bells & Mallets", "FX" })
    {
        juce::PopupMenu sub;
        const juce::String tag = juce::String ("SS:") + folder;
        for (int i = 0; i < (int) all.size(); ++i)
            if (all[(size_t) i].cat != nullptr && tag == all[(size_t) i].cat) sub.addItem (2000 + i, all[(size_t) i].name);
        store.addSubMenu (folder, sub);
    }
    pool.addSubMenu ("Sonic Store", store);
}
static const std::vector<PoolInstrument>& instrumentPool()
{
    static const std::vector<PoolInstrument> pool = withSerumPack ({
        //  name                 colour      wv  a      d     s     r     cut    res   det  uni drv   fmr  fmi  mor  noi   lfo   vib   glide mono   root
        { "Prism (morph pad)",   0xffb06bff, 7, 0.8f,  1.0f, 0.8f, 1.6f, 6500,  0.3f, 0,   1, 0,    1,   0,   0.3f,0,    0.15f,0.04f,0,    false, 60 },
        { "Drift (supersaw)",    0xff5ad1ff, 5, 0.01f, 0.4f, 0.8f, 0.35f,9000,  0.25f,0.5f,1, 0.1f, 1,   0,   0,   0,    5.5f, 0.12f,0.05f,false, 60 },
        { "Lava (squelch bass)", 0xffff6a3d, 0, 0.002f,0.35f,0.5f, 0.12f,900,   0.75f,0.2f,1, 0.45f,1,   0,   0,   0,    0,    0,    0.07f,true,  36 },
        { "Glass (FM bells)",    0xff8affe0, 6, 0.001f,1.6f, 0,    1.2f, 14000, 0.1f, 0,   1, 0,    3.5f,4,   0,   0,    0,    0,    0,    false, 72 },
        { "Haze (lo-fi keys)",   0xffe8c27a, 6, 0.005f,1.2f, 0.35f,0.6f, 4200,  0.15f,0,   1, 0.15f,1,   1.2f,0,   0.04f,0.7f, 0.09f,0,    false, 60 },
        { "Nebula (drone)",      0xff6a7dff, 8, 2.0f,  2.0f, 1.0f, 3.0f, 2800,  0.5f, 0,   1, 0,    1,   0,   0.2f,0.5f, 0.08f,0.03f,0,    false, 48 },
        { "Acoustic Piano",      0xfff2f2f2, 6, 0.001f,2.6f, 0,    0.45f,9000,  0.05f,0,   1, 0,    1,   2.8f,0,   0,    0,    0,    0,    false, 60 },
        { "Electric Piano",      0xffffb35a, 6, 0.001f,2.2f, 0.25f,0.5f, 7000,  0.1f, 0,   1, 0.05f,1,   1.8f,0,   0,    0,    0,    0,    false, 60 },
        { "Rhodes",              0xffff8a5a, 6, 0.002f,2.5f, 0.3f, 0.6f, 5500,  0.1f, 0,   1, 0.1f, 1,   1.3f,0,   0,    5,    0.03f,0,    false, 60 },
        { "Guitar Pluck",        0xffd9a066, 0, 0.001f,0.7f, 0,    0.25f,3200,  0.35f,0.15f,1,0.1f, 1,   0,   0,   0,    0,    0,    0,    false, 60 },
        { "Bass Guitar",         0xff9a6a3d, 2, 0.002f,0.9f, 0.35f,0.15f,1600,  0.3f, 0,   1, 0.25f,1,   0,   0,   0,    0,    0,    0.03f,true,  36 },
        { "Strings",             0xffc9a0ff, 5, 0.45f, 1.0f, 0.9f, 0.9f, 5000,  0.15f,0.35f,1,0,    1,   0,   0,   0,    5,    0.08f,0,    false, 60 },
        { "Brass Stab",          0xffffd04a, 0, 0.03f, 0.35f,0.55f,0.25f,3800,  0.35f,0.3f,2, 0.2f, 1,   0,   0,   0,    0,    0,    0,    false, 60 },
        { "Flute",               0xffa0e8ff, 3, 0.08f, 0.3f, 0.85f,0.25f,6000,  0.05f,0,   1, 0,    1,   0,   0,   0.08f,5,    0.12f,0,    false, 72 },
        { "Choir Pad",           0xffe0d0ff, 7, 0.6f,  1.2f, 0.85f,1.4f, 4200,  0.25f,0,   1, 0,    1,   0,   0.12f,0.03f,0.3f, 0.04f,0,    false, 60 },
        // 3x Oscillation                                                                                          shapes      levels             coarse          fine
        { "3x Saw Stack (lead)", 0xffff9a3d, 9, 0.005f,0.4f, 0.8f, 0.3f, 9000,  0.2f, 0,   1, 0.1f, 1,   0,   0,   0,    0,    0,    0,    false, 60, 0, 0, { 0, 0, 0 }, { 1, 0.8f, 0.6f }, { 0, 0, -12 }, { 0, 12, -9 } },
        { "3x Sub Bass",         0xffff5d8a, 9, 0.002f,0.6f, 0.7f, 0.15f,1800,  0.25f,0,   1, 0.2f, 1,   0,   0,   0,    0,    0,    0.05f,true,  36, 0, 0, { 3, 2, 0 }, { 1, 0.6f, 0.15f }, { 0, -12, 12 }, { 0, 0, 0 } },
        { "3x Hollow Square",    0xff6ad1ff, 9, 0.01f, 0.5f, 0.75f,0.35f,5000,  0.3f, 0,   1, 0,    1,   0,   0,   0,    0,    0,    0,    false, 60, 0, 0, { 1, 1, 3 }, { 0.9f, 0.6f, 0.4f }, { 0, 7, 12 }, { 0, 5, 0 } },
        { "3x Pluck Keys",       0xfff2d35a, 9, 0.001f,0.45f,0,    0.3f, 6000,  0.2f, 0,   1, 0,    1,   0,   0,   0,    0,    0,    0,    false, 60, 0, 0, { 2, 0, 3 }, { 1, 0.4f, 0.5f }, { 0, 12, 19 }, { 0, 3, 0 } },
        { "3x Noise Snare",      0xffd0d0d0, 9, 0.001f,0.18f,0,    0.12f,8000,  0.1f, 0,   1, 0.2f, 1,   0,   0,   0,    0,    0,    0,    false, 48, 0, 0, { 3, 4, 2 }, { 1, 0.7f, 0.3f }, { 0, 0, 12 }, { 0, 0, 0 } },
        { "3x Fifths Pad",       0xffa58aff, 9, 0.7f,  1.0f, 0.85f,1.4f, 4000,  0.2f, 0,   1, 0,    1,   0,   0,   0,    0.2f, 0.03f,0,    false, 60, 0, 0, { 0, 2, 0 }, { 0.8f, 0.7f, 0.6f }, { 0, 7, 12 }, { -7, 0, 7 } },
        // HumWave (wavetable): table, position; Morph = how far the LFO sweeps the position
        { "HumWave Growl Bass",  0xffff3d6a, 10,0.002f,0.5f, 0.8f, 0.15f,3000,  0.4f, 0,   1, 0.3f, 1,   0,   0.5f,0,    2.0f, 0,    0.05f,true,  36, 3, 0.4f },
        { "HumWave Vowel Lead",  0xffffb0e0, 10,0.01f, 0.4f, 0.8f, 0.3f, 9000,  0.2f, 0.2f,1, 0.1f, 1,   0,   0.6f,0,    0.8f, 0.05f,0.04f,false, 60, 2, 0.3f },
        { "HumWave Saw Sweep",   0xff5ad1ff, 10,0.01f, 0.4f, 0.85f,0.4f, 12000, 0.2f, 0.5f,1, 0,    1,   0,   0.3f,0,    0.1f, 0,    0,    false, 60, 0, 0.8f },
        { "HumWave Glass Pluck", 0xff8affe0, 10,0.001f,0.5f, 0,    0.4f, 14000, 0.1f, 0,   1, 0,    1,   0,   0,   0,    0,    0,    0,    false, 72, 6, 0.6f },
        { "HumWave PWM Pad",     0xffb06bff, 10,0.8f,  1.0f, 0.9f, 1.5f, 6000,  0.2f, 0.3f,1, 0,    1,   0,   0.8f,0,    0.25f,0.03f,0,    false, 60, 1, 0.5f },
        { "HumWave Digital Arp", 0xff7dff8a, 10,0.001f,0.25f,0,    0.15f,10000, 0.2f, 0,   1, 0,    1,   0,   0,   0,    0,    0,    0,    false, 60, 4, 0.5f },
        { "HumWave Sync Scream", 0xffff6a3d, 10,0.005f,0.4f, 0.8f, 0.25f,8000,  0.35f,0.2f,1, 0.4f, 1,   0,   0.7f,0,    4.0f, 0,    0.03f,true,  48, 5, 0.2f },
        { "HumWave Metal Bell",  0xffe8e8ff, 10,0.001f,1.5f, 0,    1.2f, 14000, 0.1f, 0,   1, 0,    1,   0,   0,   0,    0,    0,    0,    false, 72, 7, 0.5f },
    });
    return pool;
}
static Channel poolChannel (const PoolInstrument& e)
{
    Channel c; c.type = ChannelType::Synth; c.mixerTrack = -1;
    c.name = juce::String (e.name).upToFirstOccurrenceOf (" (", false, false);
    c.colour = juce::Colour (e.colour);
    auto& sp = c.synth;
    sp.wave = e.wave; sp.attack = e.a; sp.decay = e.d; sp.sustain = e.s; sp.release = e.r; sp.cutoff = e.cut; sp.resonance = e.res;
    sp.detune = e.det; sp.unison = e.uni; sp.drive = e.drv; sp.fmRatio = e.fmr; sp.fmIndex = e.fmi; sp.morph = e.mor; sp.noise = e.noi;
    sp.lfoRate = e.lfo; sp.vibrato = e.vib;
    sp.wtKind = e.table; sp.wtPos = e.pos; sp.warp = e.warp; sp.warpAmt = e.warpAmt; sp.uniBlend = e.blend; sp.sub = e.sub;
    sp.xWave = e.xw; sp.xVol = e.xv; sp.xSemi = e.xs; sp.xFine = e.xf;
    c.glide = e.glide; c.mono = e.mono; c.rootKey = e.root;
    return c;
}
// picture to sound: each row of the picture is one wavetable frame, brightness is the wave height
static std::shared_ptr<const std::vector<float>> pictureTable (const juce::File& f)
{
    auto img = juce::ImageFileFormat::loadFrom (f);
    if (! img.isValid()) return nullptr;
    constexpr int frames = 64;
    img = img.rescaled (humWaveFrameSize, frames, juce::Graphics::highResamplingQuality);
    std::vector<float> t ((size_t) (frames * humWaveFrameSize));
    for (int y = 0; y < frames; ++y)
        for (int x = 0; x < humWaveFrameSize; ++x)
            t[(size_t) (y * humWaveFrameSize + x)] = img.getPixelAt (x, y).getPerceivedBrightness() * 2.0f - 1.0f;
    return humWaveFromRaw (std::move (t));
}
static void makeHumWave (Channel& n, std::shared_ptr<const std::vector<float>> table, const juce::String& from)
{
    n.type = ChannelType::Synth; n.name = "HumWave " + from; n.colour = juce::Colour (0xffb06bff);
    n.synth.wave = 10; n.synth.wt = std::move (table); n.synth.wtName = from; n.synth.wtPos = 0.0f;
    n.synth.morph = 0.4f; n.synth.lfoRate = 0.3f; n.synth.sustain = 0.8f; n.synth.release = 0.4f;
}

static bool isMelodicSample (const juce::File& f)   // notes change the pitch (root key plays the WAV as-is); drum hits stay put
{
    const auto n = f.getFileNameWithoutExtension().toLowerCase(), dir = f.getParentDirectory().getFileName().toLowerCase();
    if (n.contains ("808") || dir.contains ("808") || f.getFullPathName().containsIgnoreCase ("Instruments")) return true;
    return ! isDrumSampleName (n) && ! isDrumSampleName (dir);   // a kit's Hi-Hats / Snares folder keeps every hit at its own pitch
}
static bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"); }

void addSampleChannels (Studio& s, const juce::Array<juce::File>& files, const juce::String& insertName)
{
    s.edit ("Add samples", [&] (Project& p) {
        int insert = -1;
        if (insertName.isNotEmpty())
        {
            for (int i = 1; i < (int) p.mixer.size(); ++i) if (p.mixer[(size_t) i].name == insertName) insert = i;
            if (insert < 0) insert = p.addMixerTrack (insertName);
        }
        for (auto& f : files)
        {
            Channel c;
            c.name = f.getFileNameWithoutExtension();
            c.type = ChannelType::Sampler;
            c.sampleFile = f;
            c.keyTrack = isMelodicSample (f);
            c.mixerTrack = insert;
            c.colour = juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.55f, 0.95f, 1.0f);
            s.selectedChannel = p.addChannel (c);
        }
    });
}

void importKit (Studio& s, const juce::File& folder)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        if (isAudioFile (e.getFile())) { files.add (e.getFile()); if (files.size() >= 128) break; }
    files.sort();
    if (! files.isEmpty()) addSampleChannels (s, files, folder.getFileName());
}

void addPluginChannel (Studio& s, const juce::PluginDescription& d)
{
    if (s.dirty) writeBackup (s.p());
    s.edit ("Add " + d.name, [&] (Project& p) {
        Channel c; c.name = d.name; c.type = ChannelType::Plugin; c.pluginId = d.createIdentifierString(); c.mixerTrack = -1;
        c.colour = col::accent2;
        s.selectedChannel = p.addChannel (c);
    });
    openChannelEditor (s, s.selectedChannel);
}

namespace
{
constexpr int rowH = 34, headerH = 34, ctlW = 352;

class ChannelRack;

struct NameTag : public juce::Component, public juce::SettableTooltipClient
{
    std::function<void (const juce::MouseEvent&)> onMouse;
    std::function<void()> onDouble;
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onDouble) onDouble(); }
    juce::String text; juce::Colour colour; bool selected = false;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1);
        g.setColour (selected ? colour.withAlpha (0.9f) : col::panel2);
        g.fillRoundedRectangle (r, 4);
        g.setColour (colour); g.fillRect (r.removeFromLeft (4));
        g.setColour (selected ? juce::Colours::black : col::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        if (selected) g.drawFittedText (text, getLocalBounds().reduced (8, 0), juce::Justification::centredLeft, 1);
        else drawHeadline (g, text, getLocalBounds().reduced (8, 0), juce::Justification::centredLeft, true);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (onMouse && contains (e.getPosition())) onMouse (e); }
    std::function<void (int)> onShuffle;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (onShuffle && w.deltaY != 0) onShuffle (w.deltaY > 0 ? -1 : 1); else Component::mouseWheelMove (e, w);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isTypingNoteKey (k)) return false;
        if (onShuffle && k == juce::KeyPress::upKey) { onShuffle (-1); return true; }
        if (onShuffle && k == juce::KeyPress::downKey) { onShuffle (1); return true; }
        return false;
    }
};

// FL-style sample shuffling: step to the previous / next audio file in the same folder
static juce::File siblingSample (const juce::File& f, int dir)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (f.getParentDirectory(), false, "*", juce::File::findFiles))
        if (isAudioFile (e.getFile())) files.add (e.getFile());
    files.sort();
    const int i = files.indexOf (f);
    if (files.isEmpty()) return f;
    return files[(i < 0 ? 0 : i + dir + files.size()) % files.size()];
}

struct Row : public juce::Component
{
    Row (Studio& st, int id) : s (st), channelId (id)
    {
        setupButton (mute, {}, "Channel on/off (green = on)", true);
        mute.setColour (juce::TextButton::buttonOnColourId, col::good);
        mute.onClick = [this] { const bool on = mute.getToggleState(); s.edit ("Mute channel", [&] (Project&) { if (auto* c = s.channel (channelId)) c->muted = ! on; }); };
        bind (vol, [] (Channel& c, float v) { c.volume = v; });
        bind (pan, [] (Channel& c, float v) { c.pan = v; });
        bind (pitch, [] (Channel& c, float v) { c.pitch = v; });
        glide.setSkewFactorFromMidPoint (0.15);
        bind (glide, [] (Channel& c, float v) { c.glide = v; });
        bind (shift, [] (Channel& c, float v) { c.shift = v; });
        insert.setSliderStyle (juce::Slider::LinearBar);
        insert.setTooltip ("Mixer insert this channel plays through (drag to change, 0 = master)");
        insert.onValueChange = [this] { s.edit ("Route channel", [&] (Project&) { if (auto* c = s.channel (channelId)) c->mixerTrack = (int) insert.getValue(); }); };
        name.onMouse = [this] (const juce::MouseEvent& e) { e.mods.isPopupMenu() ? menu() : select (true); };
        setupButton (wrench, "SET", "Channel settings (FL wrench): normalize, reverse, trim, fades, declick, ADSR, cut itself, stretch", false);
        wrench.onClick = [this] { openChannelEditor (s, channelId); };
        addAndMakeVisible (wrench);
        setupButton (link, "MIDI", "Link MIDI: your keyboard / piano plays this channel (link as many as you like; none linked = the selected channel plays)", true);
        link.setColour (juce::TextButton::buttonOnColourId, col::accent);
        link.onClick = [this] { const bool on = link.getToggleState(); s.edit ("Link MIDI", [&] (Project&) { if (auto* c = s.channel (channelId)) c->midiLink = on; }); };
        addAndMakeVisible (link);
        for (juce::Component* c : { (juce::Component*) &mute, (juce::Component*) &vol, (juce::Component*) &pan, (juce::Component*) &pitch,
                                    (juce::Component*) &glide, (juce::Component*) &shift, (juce::Component*) &name, (juce::Component*) &insert })
            addAndMakeVisible (c);
        name.onDouble = [this] { openChannelEditor (s, channelId); };   // FL: wave, amp / INS, sample settings in one window
        name.setTooltip ("Click: open in Piano Roll. Double-click: channel settings (wave, amp, sample). Drop a sample here: replace this channel's sound. Mouse wheel / Up-Down: shuffle samples in this folder. Right-click: rename, delete, editor, routing");
        name.setWantsKeyboardFocus (true);
        name.onShuffle = [this] (int dir) {
            auto* c = s.channel (channelId);
            if (c == nullptr || c->type != ChannelType::Sampler || ! c->sampleFile.existsAsFile()) return;
            const auto next = siblingSample (c->sampleFile, dir);
            if (next == c->sampleFile) return;
            s.edit ("Shuffle sample", [&] (Project&) { if (auto* ch = s.channel (channelId)) { ch->sampleFile = next; ch->name = next.getFileNameWithoutExtension(); } });
            if (! s.engine.isPlaying()) s.engine.previewFile (next);
        };
        refresh();
    }
    void bind (Knob& k, std::function<void (Channel&, float)> set)
    {
        k.onValueChange = [this, &k, set] {
            s.live ([&] (Project&) { if (auto* c = s.channel (channelId)) set (*c, (float) k.getValue()); });
            if (! k.isMouseButtonDown()) s.commit (k.getName());
        };
        k.onDragEnd = [this, &k] { s.commit (k.getName()); };
    }
    void refresh()
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        mute.setToggleState (! c->muted, juce::dontSendNotification);
        link.setToggleState (c->midiLink, juce::dontSendNotification);
        vol.setValue (c->volume, juce::dontSendNotification); pan.setValue (c->pan, juce::dontSendNotification);
        pitch.setValue (c->pitch, juce::dontSendNotification); glide.setValue (c->glide, juce::dontSendNotification);
        shift.setValue (c->shift, juce::dontSendNotification);
        insert.setRange (0, juce::jmax (1, (int) s.p().mixer.size() - 1), 1);
        insert.setValue (c->mixerTrack, juce::dontSendNotification);
        name.text = c->name; name.colour = c->colour; name.selected = s.selectedChannel == channelId;
        name.repaint();
    }
    void select (bool openRoll)
    {
        s.selectedChannel = channelId;
        if (auto* c = s.channel (channelId)) if (c->mixerTrack > 0) s.selectedTrack = c->mixerTrack;
        s.sendChangeMessage();
        if (openRoll && s.showView) s.showView (0);
    }
    void menu()
    {
        juce::PopupMenu m;
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        m.addItem (11, "Patch Method (stack effects on this sound)...");
        juce::PopupMenu hw;
        hw.addItem (12, "Picture to sound (any image becomes a wavetable)...");
        hw.addItem (13, "Turn this sample into a wavetable", c->type == ChannelType::Sampler && c->sampleFile.existsAsFile());
        hw.addItem (14, "Load a sample / kit one-shot as wavetable...");
        m.addSubMenu ("HumWave", hw);
        m.addSeparator();
        m.addItem (1, "Open in Piano Roll");
        m.addItem (2, c->type == ChannelType::Plugin ? "Open plugin editor" : "Channel settings (synth / sampler)...");
        m.addItem (3, "Rename...");
        m.addItem (4, "Route to selected mixer insert (" + juce::String (s.selectedTrack) + ")");
        m.addItem (5, "New mixer insert for this channel");
        m.addItem (10, "Change sound...");
        m.addItem (6, "Clear notes in this pattern");
        m.addItem (8, "Cut itself (808s never overlap)", c->type == ChannelType::Sampler, c->cutItself);
        m.addItem (9, "One-shot (play whole sample)", c->type == ChannelType::Sampler, c->oneShot);
        m.addSubMenu ("Color", colourMenu (30));
        m.addSeparator();
        m.addItem (7, "Delete channel");
        m.showMenuAsync ({}, [this] (int r) {
            const int id = channelId;
            if (r == 1) select (true);
            else if (r == 11) openPatchMethod (s, id);
            else if (r == 12 || r == 14)
            {
                const bool pic = r == 12;
                chooser = std::make_unique<juce::FileChooser> (pic ? "Picture to sound" : "Sample to wavetable",
                                                               juce::File::getSpecialLocation (pic ? juce::File::userPicturesDirectory : juce::File::userMusicDirectory),
                                                               pic ? "*.png;*.jpg;*.jpeg;*.gif;*.bmp" : "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, pic] (const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    std::shared_ptr<const std::vector<float>> t;
                    if (pic) t = pictureTable (f);
                    else if (auto smp = loadSampleFile (f, 44100.0)) t = humWaveFromAudio (*smp);
                    if (t == nullptr) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "HumWave", "Couldn't read " + f.getFileName()); return; }
                    replaceSound ([&] (Channel& n) { makeHumWave (n, t, f.getFileNameWithoutExtension()); });
                    openChannelEditor (s, channelId);
                });
            }
            else if (r == 13)
            {
                auto* ch = s.channel (id);
                if (ch == nullptr) return;
                const auto f = ch->sampleFile;
                if (auto smp = loadSampleFile (f, 44100.0))
                {
                    auto t = humWaveFromAudio (*smp);
                    replaceSound ([&] (Channel& n) { makeHumWave (n, t, f.getFileNameWithoutExtension()); });
                    openChannelEditor (s, id);
                }
            }
            else if (r == 10) changeSoundMenu();
            else if (r == 2) openChannelEditor (s, id);
            else if (r == 3) { auto* c = s.channel (id); if (c) askText ("Rename channel", c->name, [this, id] (juce::String t) { s.edit ("Rename", [&] (Project&) { if (auto* ch = s.channel (id)) ch->name = t; }); }); }
            else if (r == 4) s.edit ("Route channel", [&] (Project&) { if (auto* ch = s.channel (id)) ch->mixerTrack = s.selectedTrack; });
            else if (r == 5) s.edit ("New insert", [&] (Project& p) { if (auto* ch = s.channel (id)) ch->mixerTrack = p.addMixerTrack (ch->name); });
            else if (r == 8) s.edit ("Cut itself", [&] (Project&) { if (auto* ch = s.channel (id)) ch->cutItself = ! ch->cutItself; });
            else if (r == 9) s.edit ("One-shot", [&] (Project&) { if (auto* ch = s.channel (id)) ch->oneShot = ! ch->oneShot; });
            else if (r == 6) s.edit ("Clear notes", [&] (Project&) { s.notes (id).clear(); });
            else if (r >= 30 && r <= 42) { const auto c = pickedColour (r - 30); s.edit ("Channel color", [&] (Project&) { if (auto* ch = s.channel (id)) ch->colour = c; }); }
            else if (r == 7) s.edit ("Delete channel", [&] (Project& p) {
                p.channels.erase (std::remove_if (p.channels.begin(), p.channels.end(), [id] (const Channel& ch) { return ch.id == id; }), p.channels.end());
                for (auto& pt : p.patterns) pt.notes.erase (id);
            });
        });
    }
    void replaceSound (const std::function<void (Channel&)>& make)
    {
        const int id = channelId;
        s.edit ("Change sound", [&] (Project&) {
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            Channel nc; make (nc);
            nc.id = ch->id; nc.mixerTrack = ch->mixerTrack; nc.volume = ch->volume; nc.pan = ch->pan; nc.muted = ch->muted; nc.solo = ch->solo;
            *ch = nc;
        });
        s.selectedChannel = id;
        s.sendChangeMessage();
    }
    void changeSoundMenu()
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        juce::PopupMenu m, pool, inst;
        m.addSectionHeader ("Change sound - " + c->name);
        m.addItem (1, "Sample...");
        m.addItem (2, "Synth (keys / lead)");
        m.addItem (3, "808 (glide bass)");
        pool.addSectionHeader ("Psychedelic synths");
        for (int i = 0; i < (int) instrumentPool().size(); ++i)
        {
            if (instrumentPool()[(size_t) i].cat != nullptr) continue;
            if (i == 6) pool.addSectionHeader ("Live instruments");
            if (juce::String (instrumentPool()[(size_t) i].name).startsWith ("3x ") && ! juce::String (instrumentPool()[(size_t) (i - 1)].name).startsWith ("3x "))
                pool.addSectionHeader ("3x Oscillation");
            if (juce::String (instrumentPool()[(size_t) i].name).startsWith ("HumWave") && ! juce::String (instrumentPool()[(size_t) (i - 1)].name).startsWith ("HumWave"))
                pool.addSectionHeader ("HumWave (wavetable)");
            pool.addItem (2000 + i, instrumentPool()[(size_t) i].name);
        }
        addSerumPackMenu (pool, instrumentPool());
        m.addSubMenu ("Instrument Pool", pool);
        auto types = s.engine.knownPlugins.getTypes();
        for (int i = 0; i < types.size(); ++i) if (types.getReference (i).isInstrument) inst.addItem (1000 + i, types.getReference (i).name + " (" + types.getReference (i).manufacturerName + ")");
        m.addSubMenu ("Plugin instrument", inst, inst.getNumItems() > 0);
        const auto startDir = c->sampleFile.existsAsFile() ? c->sampleFile.getParentDirectory()
                                                           : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
        m.showMenuAsync ({}, [this, types, startDir] (int r) {
            if (r == 1)
            {
                chooser = std::make_unique<juce::FileChooser> ("Change sample", startDir, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    replaceSound ([&] (Channel& n) { n.type = ChannelType::Sampler; n.sampleFile = f; n.keyTrack = isMelodicSample (f); n.name = f.getFileNameWithoutExtension(); });
                    if (! s.engine.isPlaying()) s.engine.previewFile (f);
                });
            }
            else if (r == 2) replaceSound ([] (Channel& n) { n.type = ChannelType::Synth; n.name = "Synth"; n.colour = juce::Colour (0xff8a7dff); });
            else if (r == 3) replaceSound ([] (Channel& n) { n.type = ChannelType::Synth; n.name = "808"; n.synth.wave = 4; n.glide = 0.08f; n.mono = true; n.rootKey = 36; n.colour = juce::Colour (0xffff5d8a); });
            else if (r >= 2000 && r < 2000 + (int) instrumentPool().size())
                replaceSound ([r] (Channel& n) { n = poolChannel (instrumentPool()[(size_t) (r - 2000)]); });
            else if (r >= 1000 && r - 1000 < types.size())
            {
                const auto d = types[r - 1000];
                if (s.dirty) writeBackup (s.p());
                replaceSound ([&d] (Channel& n) { n.type = ChannelType::Plugin; n.name = d.name; n.pluginId = d.createIdentifierString(); n.colour = col::accent2; });
                openChannelEditor (s, channelId);
            }
        });
    }
    std::unique_ptr<juce::FileChooser> chooser;
    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        mute.setBounds (r.removeFromLeft (16).reduced (0, 6)); r.removeFromLeft (4);
        for (auto* k : { &vol, &pan, &pitch, &glide, &shift }) k->setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        insert.setBounds (r.removeFromRight (34).reduced (0, 4));
        wrench.setBounds (r.removeFromRight (30).reduced (1, 4));
        link.setBounds (r.removeFromRight (34).reduced (1, 4));
        name.setBounds (r.reduced (2, 1));
    }
    Studio& s; int channelId;
    juce::TextButton mute, wrench, link;
    Knob vol { "Volume", "channel level", 0.0, 1.25, 0.8 }, pan { "Pan", "left / right", -1.0, 1.0, 0.0 },
         pitch { "Pitch", "transpose in semitones", -24.0, 24.0, 0.0, " st" },
         glide { "Glide", "slide time between notes (808s, melodies); 0 = off", 0.0, 1.0, 0.0, " s" },
         shift { "Shift", "play this channel's hits off the grid: + late (laid back), - early (pushed); 0 = on the grid", -60.0, 60.0, 0.0, " ms" };
    NameTag name;
    juce::Slider insert;
};

class ChannelRack : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget, public juce::TooltipClient,
                    private juce::ChangeListener, private juce::Timer
{
public:
    explicit ChannelRack (Studio& st) : s (st)
    {
        s.addChangeListener (this);
        setupButton (addBtn, "+ Channel", "Add a sampler, synth, 808 or plugin instrument");
        addBtn.onClick = [this] { addMenu(); };
        setupButton (kitBtn, "Import Kit", "Pick a drum kit folder: every sample becomes a channel (or drag folders / samples onto the rack)");
        kitBtn.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> ("Choose a drum kit folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().isDirectory()) importKit (s, fc.getResult()); });
        };
        patternBox.setTooltip ("Pattern being edited (pick 'New pattern' to add one)");
        patternBox.onChange = [this] {
            const int i = patternBox.getSelectedItemIndex();
            if (i == (int) s.p().patterns.size())
                s.edit ("New pattern", [] (Project& p) {
                    Pattern pt; pt.name = "Pattern " + juce::String (p.patterns.size() + 1);
                   #if HHS_LICENSED
                    p.patterns.push_back (pt); p.currentPattern = (int) p.patterns.size() - 1;
                   #else
                    p.currentPattern = p.insertPatternAfter (p.currentPattern, pt);   // stacks right after the pattern you were editing
                   #endif
                });
            else if (i >= 0 && i != s.p().currentPattern)
                s.edit ("Select pattern", [i] (Project& p) { p.currentPattern = i; });
        };
        swing.onValueChange = [this] { s.live ([this] (Project& p) { p.swing = swing.getValue(); }); if (! swing.isMouseButtonDown()) s.commit ("Swing"); };
        swing.onDragEnd = [this] { s.commit ("Swing"); };
        for (juce::Component* c : { (juce::Component*) &addBtn, (juce::Component*) &kitBtn, (juce::Component*) &patternBox, (juce::Component*) &swing })
            addAndMakeVisible (c);
        refresh();
        startTimerHz (30);
    }
    ~ChannelRack() override { s.removeChangeListener (this); }

    juce::String getTooltip() override
    {
        if (lastMouse.x >= ctlW) return "Steps: click on/off, drag to paint, right-click = snare / hi-hat roll (x2 - x8). Drop kits or samples here";
        return {};
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::bg); g.fillRect (0, 0, getWidth(), headerH);
        g.setColour (col::dim); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        drawHeadline (g, "CHANNEL RACK", { getWidth() - 140, 0, 130, headerH }, juce::Justification::centredRight);
        auto& p = s.p();
        if (p.patterns.empty()) return;
        auto& pat = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
        const int steps = numSteps();
        const float w = stepW();
        double localPos = -1.0;
        {
            const double plen = pat.lengthBeats(), pos = s.engine.positionBeats();
            if (! p.songMode) localPos = std::fmod (pos, plen);
            else for (auto& c : p.clips)
                if (! c.isAudio() && c.patternIndex == p.currentPattern && pos >= c.start && pos < c.start + c.length) localPos = std::fmod (pos - c.start, plen);
        }
        const int playStep = s.engine.isPlaying() && localPos >= 0 ? (int) (localPos / 0.25) : -1;
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            const int y = headerH + (int) ri * rowH - scrollY;
            if (y + rowH < headerH || y > getHeight()) continue;
            auto* ch = s.channel (rows[ri]->channelId);
            if (ch == nullptr) continue;
            auto it = pat.notes.find (ch->id);
            for (int st = 0; st < steps; ++st)
            {
                juce::Rectangle<float> r ((float) ctlW + st * w + 1, (float) y + 4, w - 2, (float) rowH - 8);
                const bool group = (st / 4) % 2 == 0;
                int count = 0;
                if (it != pat.notes.end())
                    for (auto& n : it->second) if (n.start >= st * 0.25 - 1e-6 && n.start < (st + 1) * 0.25 - 1e-6) ++count;
                g.setColour (count > 0 ? ch->colour : (group ? col::panel2.brighter (0.06f) : col::grid));
                g.fillRoundedRectangle (r, 3);
                if (count > 1)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    for (int k = 1; k < count; ++k) g.fillRect (r.getX() + r.getWidth() * k / count, r.getY() + 3, 1.5f, r.getHeight() - 6);
                }
                if (st == playStep)
                {
                    if (count > 0)   // FL-style: the step lights up as it plays
                    {
                        g.setColour (ch->colour.brighter (0.9f).withAlpha (0.35f)); g.fillRoundedRectangle (r.expanded (2.0f), 4);
                        g.setColour (ch->colour.brighter (0.7f)); g.fillRoundedRectangle (r, 3);
                    }
                    g.setColour (col::text); g.drawRoundedRectangle (r, 3, 1.5f);
                }
            }
        }
        if (localPos >= 0)   // playhead line across the whole rack
        {
            const float x = (float) ctlW + (float) (localPos / 0.25) * w;
            const float y0 = (float) headerH, y1 = (float) juce::jmin (getHeight(), headerH + (int) rows.size() * rowH - scrollY);
            g.setColour (col::accent2.withAlpha (0.22f)); g.fillRect (x - 2.5f, y0, 5.0f, y1 - y0);
            g.setColour (col::accent2); g.fillRect (x - 0.75f, y0, 1.5f, y1 - y0);
        }
    }

    void resized() override
    {
        auto top = getLocalBounds().removeFromTop (headerH).reduced (6, 5);
        addBtn.setBounds (top.removeFromLeft (90)); top.removeFromLeft (4);
        kitBtn.setBounds (top.removeFromLeft (90)); top.removeFromLeft (8);
        patternBox.setBounds (top.removeFromLeft (150)); top.removeFromLeft (8);
        swing.setBounds (top.removeFromLeft (28));
        layoutRows();
    }

    void mouseMove (const juce::MouseEvent& e) override { lastMouse = e.getPosition(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        int row, step;
        if (! hitStep (e.getPosition(), row, step)) return;
        const int id = rows[(size_t) row]->channelId;
        if (e.mods.isPopupMenu()) { rollMenu (id, step); return; }
        paintOn = ! stepOn (id, step);
        setStep (id, step, paintOn);
        lastStep = { row, step };
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        int row, step;
        if (e.mods.isPopupMenu() || ! hitStep (e.getPosition(), row, step) || juce::Point<int> (row, step) == lastStep) return;
        lastStep = { row, step };
        setStep (rows[(size_t) row]->channelId, step, paintOn);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu() && e.getMouseDownX() >= ctlW) s.commit ("Steps"); lastStep = { -1, -1 }; }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        scrollY = juce::jlimit (0, juce::jmax (0, (int) rows.size() * rowH - (getHeight() - headerH)), scrollY - (int) (w.deltaY * 120));
        layoutRows(); repaint();
    }

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString() == "sample"; }
    void itemDropped (const SourceDetails& d) override   // drag from the Samples browser
    {
        if (auto* t = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
        {
            juce::StringArray files;
            for (int i = 0; i < t->getNumSelectedFiles(); ++i) files.add (t->getSelectedFile (i).getFullPathName());
            if (! files.isEmpty()) filesDropped (files, d.localPosition.x, d.localPosition.y);
        }
    }
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! allHumHouseShareFiles (files); }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        juce::Array<juce::File> samples;
        for (auto& f : files)
        {
            juce::File file (f);
            if (file.isDirectory()) importKit (s, file);
            else if (isAudioFile (file)) samples.add (file);
            else if (file.hasFileExtension ("mid;midi"))
                s.edit ("Add pattern", [file] (Project& p) { if (const int i = importMidiPattern (p, file); i >= 0) p.currentPattern = i; });
        }
        if (samples.size() == 1 && replaceSampleAt ({ x, y }, samples[0])) return;
        if (! samples.isEmpty()) addSampleChannels (s, samples, samples.size() > 1 ? "Samples" : juce::String());
    }
    // FL: a sample dropped onto a channel swaps that channel's sound; its notes, steps, routing and settings stay
    bool replaceSampleAt (juce::Point<int> pt, const juce::File& f)
    {
        if (pt.y < headerH) return false;
        const int row = (pt.y - headerH + scrollY) / rowH;
        if (row < 0 || row >= (int) rows.size()) return false;
        const int id = rows[(size_t) row]->channelId;
        auto* c = s.channel (id);
        if (c == nullptr) return false;
        s.edit ("Replace sample", [&] (Project&) {
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            if (ch->type != ChannelType::Sampler) { ch->type = ChannelType::Sampler; ch->pluginId = {}; ch->pluginState.reset(); ch->builtinDrum = -1; }
            ch->sampleFile = f;
            ch->keyTrack = isMelodicSample (f);
            ch->name = f.getFileNameWithoutExtension();
        });
        s.selectedChannel = id;
        s.sendChangeMessage();
        if (! s.engine.isPlaying()) s.engine.previewFile (f);
        return true;
    }

private:
    Studio& s;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addBtn, kitBtn;
    juce::ComboBox patternBox;
    Knob swing { "Swing", "delays every other step for groove", 0.0, 1.0, 0.0 };
    std::unique_ptr<juce::FileChooser> chooser;
    int scrollY = 0;
    bool paintOn = true;
    juce::Point<int> lastStep { -1, -1 }, lastMouse;

    int numSteps() const
    {
        auto& p = const_cast<Studio&> (s).p();
        if (p.patterns.empty()) return 16;
        return juce::jmax (16, (int) std::round (p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)].lengthBeats() * 4));
    }
    float stepW() const { return juce::jlimit (8.0f, 30.0f, (float) (getWidth() - ctlW - 8) / (float) numSteps()); }
    bool hitStep (juce::Point<int> pt, int& row, int& step) const
    {
        if (pt.x < ctlW || pt.y < headerH) return false;
        row = (pt.y - headerH + scrollY) / rowH;
        step = (int) ((pt.x - ctlW) / stepW());
        return row >= 0 && row < (int) rows.size() && step >= 0 && step < numSteps();
    }
    bool stepOn (int id, int step) { auto& p = s.p(); return p.patterns[(size_t) p.currentPattern].stepOn (id, step); }
    void setStep (int id, int step, bool on)
    {
        if (stepOn (id, step) == on) return;
        auto* ch = s.channel (id);
        if (ch == nullptr) return;
        s.live ([&] (Project& p) { p.patterns[(size_t) p.currentPattern].toggleStep (id, step, ch->rootKey); });
        if (on) s.engine.previewNote (id, ch->rootKey, 0.8f, true);
        repaint();
    }
    void rollMenu (int id, int step)
    {
        juce::PopupMenu m;
        for (int d : { 2, 3, 4, 6, 8 }) m.addItem (d, "Roll x" + juce::String (d) + (d == 3 || d == 6 ? " (triplet)" : ""));
        m.addItem (100, "Clear step");
        m.showMenuAsync ({}, [this, id, step] (int r) {
            if (r == 0) return;
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            const int key = ch->rootKey;
            s.edit (r == 100 ? "Clear step" : "Roll", [&] (Project&) {
                auto& n = s.notes (id);
                const double st = step * 0.25;
                n.erase (std::remove_if (n.begin(), n.end(), [st] (const Note& x) { return x.start >= st - 1e-6 && x.start < st + 0.25 - 1e-6; }), n.end());
                if (r != 100) tools::rollStep (n, st, 0.25, r, key, 0.8f);
            });
        });
    }
    void addMenu()
    {
        juce::PopupMenu m, inst;
        m.addItem (1, "Sampler (pick a sample)...");
        m.addItem (2, "Synth (keys / lead)");
        m.addItem (3, "808 (glide bass)");
       #if ! HHS_LICENSED
        m.addItem (4, "HumHouse Drummer (rock drums)");
       #endif
        juce::PopupMenu pool;
        pool.addSectionHeader ("Psychedelic synths");
        for (int i = 0; i < (int) instrumentPool().size(); ++i)
        {
            if (instrumentPool()[(size_t) i].cat != nullptr) continue;
            if (i == 6) pool.addSectionHeader ("Live instruments");
            if (juce::String (instrumentPool()[(size_t) i].name).startsWith ("3x ") && ! juce::String (instrumentPool()[(size_t) (i - 1)].name).startsWith ("3x "))
                pool.addSectionHeader ("3x Oscillation");
            if (juce::String (instrumentPool()[(size_t) i].name).startsWith ("HumWave") && ! juce::String (instrumentPool()[(size_t) (i - 1)].name).startsWith ("HumWave"))
                pool.addSectionHeader ("HumWave (wavetable)");
            pool.addItem (2000 + i, instrumentPool()[(size_t) i].name);
        }
        addSerumPackMenu (pool, instrumentPool());
        m.addSubMenu ("Instrument Pool", pool);
        auto types = s.engine.knownPlugins.getTypes();
        for (int i = 0; i < types.size(); ++i) if (types.getReference (i).isInstrument) inst.addItem (1000 + i, types.getReference (i).name + " (" + types.getReference (i).manufacturerName + ")");
        m.addSubMenu ("Plugin instrument", inst, inst.getNumItems() > 0);
        m.showMenuAsync ({}, [this, types] (int r) {
            if (r == 1)
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [this] (const juce::FileChooser& fc) { if (! fc.getResults().isEmpty()) addSampleChannels (s, fc.getResults(), {}); });
            }
            else if (r == 2 || r == 3)
                s.edit ("Add synth", [&] (Project& p) {
                    Channel c; c.type = ChannelType::Synth; c.mixerTrack = -1;
                    if (r == 3)
                    {
                        for (auto& e : p.channels) if (e.name.containsIgnoreCase ("808")) { c = e; c.id = 0; c.mixerTrack = -1; }
                        c.name = "808"; c.glide = juce::jmax (0.08f, c.glide); c.mono = true; c.rootKey = 36; c.type = ChannelType::Synth;
                    }
                    else c.name = "Synth";
                    c.colour = r == 3 ? juce::Colour (0xffff5d8a) : juce::Colour (0xff8a7dff);
                    s.selectedChannel = p.addChannel (c);
                });
            else if (r == 4)
                s.edit ("Add Drummer", [&] (Project& p) { s.selectedChannel = addDrummerTrack (p, 0.0, 8); });
            else if (r >= 2000 && r < 2000 + (int) instrumentPool().size())
                s.edit ("Add instrument", [&] (Project& p) { s.selectedChannel = p.addChannel (poolChannel (instrumentPool()[(size_t) (r - 2000)])); });
            else if (r >= 1000) addPluginChannel (s, types.getReference (r - 1000));
        });
    }
    void refresh()
    {
        auto& p = s.p();
        bool same = rows.size() == p.channels.size();
        for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i]->channelId == p.channels[i].id;
        if (! same)
        {
            rows.clear();
            for (auto& c : p.channels) { rows.push_back (std::make_unique<Row> (s, c.id)); addAndMakeVisible (*rows.back()); }
            layoutRows();
        }
        for (auto& r : rows) r->refresh();
        patternBox.clear (juce::dontSendNotification);
        for (size_t i = 0; i < p.patterns.size(); ++i) patternBox.addItem (p.patterns[i].name, (int) i + 1);
        patternBox.addItem ("+ New pattern", (int) p.patterns.size() + 1);
        patternBox.setSelectedItemIndex (p.currentPattern, juce::dontSendNotification);
        swing.setValue (p.swing, juce::dontSendNotification);
        if (s.selectedChannel < 0 && ! p.channels.empty()) s.selectedChannel = p.channels.front().id;
        repaint();
    }
    void layoutRows()
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int y = headerH + (int) i * rowH - scrollY;
            rows[i]->setBounds (0, y, ctlW - 6, rowH);
            rows[i]->setVisible (y + rowH > headerH);
        }
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { if (s.engine.isPlaying()) repaint (ctlW, headerH, getWidth() - ctlW, getHeight() - headerH); }
};
}

std::unique_ptr<juce::Component> createChannelRack (Studio& s) { return std::make_unique<ChannelRack> (s); }
}
