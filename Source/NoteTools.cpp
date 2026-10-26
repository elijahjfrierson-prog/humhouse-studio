#include "NoteTools.h"
#include <algorithm>
#include <random>
#include <cmath>

namespace hhs::tools
{
bool anySelected (const NoteList& l)
{
    return std::any_of (l.begin(), l.end(), [] (const Note& n) { return n.selected; });
}

void sortNotes (NoteList& l)
{
    std::stable_sort (l.begin(), l.end(), [] (const Note& a, const Note& b)
                      { return a.start != b.start ? a.start < b.start : a.key < b.key; });
}

static void splitTargets (NoteList& l, NoteList& targets, NoteList& rest)
{
    const bool sel = anySelected (l);
    for (auto& n : l) (! sel || n.selected ? targets : rest).push_back (n);
}

// group overlapping notes that start together into chords
static std::vector<NoteList> chords (NoteList notes)
{
    sortNotes (notes);
    std::vector<NoteList> out;
    for (auto& n : notes)
    {
        if (! out.empty() && std::abs (out.back().front().start - n.start) < 1.0e-6) out.back().push_back (n);
        else out.push_back ({ n });
    }
    return out;
}

void arpeggiate (NoteList& l, const ArpSettings& s)
{
    NoteList t, rest; splitTargets (l, t, rest);
    const double step = s.triplet ? s.rate * 2.0 / 3.0 : s.rate;
    for (auto& ch : chords (t))
    {
        std::vector<int> keys;
        for (int o = 0; o < std::max (1, s.range); ++o)
            for (auto& n : ch) keys.push_back (n.key + 12 * o);
        std::sort (keys.begin(), keys.end());
        if (s.direction == 1) std::reverse (keys.begin(), keys.end());
        if (s.direction == 2 && keys.size() > 2) for (int i = (int) keys.size() - 2; i > 0; --i) keys.push_back (keys[(size_t) i]);
        if (s.direction == 3) { keys.clear(); for (auto& n : ch) keys.push_back (n.key); }
        if (s.direction == 5 || s.direction == 6)   // converge: outside in / diverge: middle out
        {
            std::vector<int> o; int lo = 0, hi = (int) keys.size() - 1;
            while (lo <= hi) { o.push_back (keys[(size_t) lo++]); if (lo <= hi) o.push_back (keys[(size_t) hi--]); }
            if (s.direction == 6) std::reverse (o.begin(), o.end());
            keys = o;
        }
        if ((s.direction == 7 || s.direction == 8) && keys.size() > 1)   // pinky: top note between every note / thumb: bottom note
        {
            const int pedal = s.direction == 7 ? keys.back() : keys.front();
            std::vector<int> o;
            for (size_t k = 0; k < keys.size(); ++k)
                if (keys[k] != pedal || (s.direction == 7 ? k + 1 < keys.size() : k > 0)) { o.push_back (pedal); o.push_back (keys[k]); }
            keys = o;
        }
        juce::uint32 rng = (juce::uint32) (s.seed * 2654435761u + (juce::uint32) ch.front().key * 40503u + (juce::uint32) (ch.front().start * 960.0));
        auto rnd = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        int seq = 0;

        double len = 0.0;
        for (auto& n : ch) len = std::max (len, n.length);
        const int count = std::max (1, (int) std::floor (len / step + 1.0e-6));
        for (int i = 0; i < count; ++i)
        {
            if ((s.rests == 1 && i % 4 == 3) || (s.rests == 2 && i % 8 != 0 && i % 8 != 3 && i % 8 != 6)
                || (s.rests == 3 && rnd() % 4 == 0) || (s.rests == 4 && i % 4 == 1)) continue;
            Note n = ch.front();
            n.start = ch.front().start + i * step + ((i & 1) ? 0.5 * step * s.swing : 0.0);
            n.length = std::max (0.01, step * s.gate);
            n.slide = s.slide && (seq & 1);
            const float r = count > 1 ? (float) i / (float) (count - 1) : 0.0f;
            n.velocity = juce::jlimit (0.0f, 1.0f, ch.front().velocity * juce::jmap (r, s.velStart, s.velEnd));
            if (s.direction == 9) { for (auto& c : ch) { Note k = n; k.key = c.key; rest.push_back (k); } ++seq; continue; }
            n.key = s.direction == 4 ? keys[(size_t) (rnd() % keys.size())] : keys[(size_t) seq % keys.size()];
            ++seq;
            rest.push_back (n);
        }
    }
    l = rest; sortNotes (l);
}

void strum (NoteList& l, const StrumSettings& s)
{
    NoteList t, rest; splitTargets (l, t, rest);
    int idx = 0;
    for (auto& ch : chords (t))
    {
        std::sort (ch.begin(), ch.end(), [] (auto& a, auto& b) { return a.key < b.key; });
        bool down = s.time < 0.0;
        if (s.alternate && (idx++ % 2) == 1) down = ! down;
        if (down) std::reverse (ch.begin(), ch.end());
        const int k = (int) ch.size();
        for (int i = 0; i < k; ++i)
        {
            auto n = ch[(size_t) i];
            double r = k > 1 ? (double) i / (double) (k - 1) : 0.0;
            if (s.tension != 0.0f) r = std::pow (r, std::exp ((double) s.tension));
            const double off = std::abs (s.time) * r * (k - 1);
            const double end = n.end();
            n.start += off;
            n.length = s.preserveEnd ? std::max (0.01, end - n.start) : n.length;
            n.velocity = juce::jlimit (0.0f, 1.0f, n.velocity * (1.0f + s.velocity * (float) r));
            rest.push_back (n);
        }
    }
    l = rest; sortNotes (l);
}

void chop (NoteList& l, const ChopSettings& s)
{
    NoteList t, rest; splitTargets (l, t, rest);
    const double step = s.triplet ? s.rate * 2.0 / 3.0 : s.rate;
    for (auto& n : t)
    {
        for (double p = n.start; p < n.end() - 1.0e-6; p += step)
        {
            Note c = n; c.start = p; c.length = std::max (0.01, std::min (step, n.end() - p) * s.gate);
            rest.push_back (c);
        }
    }
    l = rest; sortNotes (l);
}

void flam (NoteList& l, const FlamSettings& s)
{
    NoteList add;
    forTargets (l, [&] (Note& n)
    {
        Note g = n; g.start = std::max (0.0, n.start - s.time); g.length = std::max (0.01, s.time * 0.9);
        g.velocity = n.velocity * s.velocity; g.selected = false;
        add.push_back (g);
    });
    l.insert (l.end(), add.begin(), add.end());
    sortNotes (l);
}

void quantize (NoteList& l, const QuantizeSettings& s)
{
    if (s.grid <= 0.0) return;
    forTargets (l, [&] (Note& n)
    {
        const double end = n.end();
        if (s.starts)
        {
            const double q = std::round (n.start / s.grid) * s.grid;
            n.start += (q - n.start) * s.strength;
            if (! s.ends) n.length = std::max (0.01, n.length);
        }
        if (s.ends)
        {
            double qe = std::round ((s.starts ? n.end() : end) / s.grid) * s.grid;
            if (qe <= n.start + 1.0e-6) qe = n.start + s.grid;
            n.length = std::max (0.01, n.length + (qe - n.end()) * s.strength);
        }
    });
    sortNotes (l);
}

void randomize (NoteList& l, const RandomSettings& s, int root, int scaleIdx, double atBeat)
{
    std::mt19937 rng ((unsigned) s.seed);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    std::vector<int> keys;
    for (int k = s.octave * 12; k < (s.octave + std::max (1, s.octaves)) * 12 + 1; ++k)
        if (inScale (k, root, scaleIdx)) keys.push_back (k);
    if (keys.empty()) return;
    for (double p = atBeat; p < atBeat + s.region - 1.0e-6; p += s.length)
    {
        if (u (rng) > s.population) continue;
        Note n; n.start = p; n.length = s.length;
        n.key = keys[(size_t) (u (rng) * (float) keys.size()) % keys.size()];
        n.velocity = juce::jmap (u (rng), s.velMin, s.velMax);
        n.selected = true;
        l.push_back (n);
    }
    sortNotes (l);
}

void limit (NoteList& l, const LimitSettings& s, int root, int scaleIdx)
{
    forTargets (l, [&] (Note& n)
    {
        while (n.key < s.lowKey) n.key += 12;
        while (n.key > s.highKey) n.key -= 12;
        if (s.toScale) n.key = snapToScale (n.key, root, scaleIdx);
    });
}

void articulate (NoteList& l, float scale)
{
    forTargets (l, [&] (Note& n) { n.length = std::max (0.01, n.length * scale); });
}

void scaleLevels (NoteList& l, const ScaleLevelSettings& s)
{
    forTargets (l, [&] (Note& n) { n.velocity = juce::jlimit (0.0f, 1.0f, n.velocity * s.multiply + s.offset); });
}

void flip (NoteList& l, bool h, bool v)
{
    bool sel = anySelected (l);
    double t0 = 1e9, t1 = -1e9; int k0 = 128, k1 = -1;
    for (auto& n : l) if (! sel || n.selected)
    { t0 = std::min (t0, n.start); t1 = std::max (t1, n.end()); k0 = std::min (k0, n.key); k1 = std::max (k1, n.key); }
    if (k1 < 0) return;
    forTargets (l, [&] (Note& n)
    {
        if (h) n.start = t0 + (t1 - n.end());
        if (v) n.key = k0 + (k1 - n.key);
    });
    sortNotes (l);
}

void claw (NoteList& l, const ClawSettings& s)
{
    NoteList t, rest; splitTargets (l, t, rest);
    sortNotes (t);
    int i = 0;
    for (auto& n : t)
    {
        const int slot = i++;
        if (s.trashEvery > 0 && s.period > 0 && (slot % s.period) == s.period - 1 && (slot / s.period) % s.trashEvery == 0) continue;
        Note c = n;
        if (s.timeDistort != 0.0f)
        {
            const double phase = std::fmod (c.start, 1.0);
            c.start = std::floor (c.start) + std::pow (phase, std::exp ((double) s.timeDistort));
        }
        rest.push_back (c);
    }
    l = rest; sortNotes (l);
}

void lfo (NoteList& l, const LfoSettings& s)
{
    forTargets (l, [&] (Note& n)
    {
        const double ph = std::fmod (n.start / std::max (0.01, s.period) + s.phase, 1.0);
        float v = 0.0f;
        switch (s.shape)
        {
            case 0: v = 0.5f + 0.5f * (float) std::sin (ph * juce::MathConstants<double>::twoPi); break;
            case 1: v = (float) (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0); break;
            default: v = ph < 0.5 ? 1.0f : 0.0f; break;
        }
        n.velocity = juce::jmap (v, s.minV, s.maxV);
    });
}

void glue (NoteList& l)
{
    NoteList t, rest; splitTargets (l, t, rest);
    std::sort (t.begin(), t.end(), [] (auto& a, auto& b) { return a.key != b.key ? a.key < b.key : a.start < b.start; });
    for (size_t i = 0; i < t.size(); ++i)
    {
        Note n = t[i];
        while (i + 1 < t.size() && t[i + 1].key == n.key && t[i + 1].start <= n.end() + 1.0e-6)
        { n.length = std::max (n.end(), t[i + 1].end()) - n.start; ++i; }
        rest.push_back (n);
    }
    l = rest; sortNotes (l);
}

void legato (NoteList& l)
{
    sortNotes (l);
    const bool sel = anySelected (l);
    for (size_t i = 0; i < l.size(); ++i)
    {
        if (sel && ! l[i].selected) continue;
        for (size_t j = i + 1; j < l.size(); ++j)
            if (l[j].start > l[i].start + 1.0e-6 && (! sel || l[j].selected)) { l[i].length = l[j].start - l[i].start; break; }
    }
}

void quickChop (NoteList& l, double grid) { chop (l, { grid > 0 ? grid : 0.25, false, 1.0f }); }

void transpose (NoteList& l, int semis)
{
    forTargets (l, [&] (Note& n) { n.key = juce::jlimit (0, 127, n.key + semis); });
}

void move (NoteList& l, double beats)
{
    double minStart = 1e9;
    forTargets (l, [&] (Note& n) { minStart = std::min (minStart, n.start); });
    if (minStart + beats < 0.0) beats = -minStart;
    forTargets (l, [&] (Note& n) { n.start += beats; });
    sortNotes (l);
}

void duplicateRight (NoteList& l)
{
    const bool sel = anySelected (l);
    double t0 = 1e9, t1 = -1e9;
    for (auto& n : l) if (! sel || n.selected) { t0 = std::min (t0, n.start); t1 = std::max (t1, n.end()); }
    if (t1 < t0) return;
    const double span = std::ceil ((t1 - t0) / 0.25 - 1.0e-6) * 0.25;
    NoteList add;
    for (auto& n : l) if (! sel || n.selected) { Note c = n; c.start += span; c.selected = true; add.push_back (c); }
    for (auto& n : l) n.selected = false;
    l.insert (l.end(), add.begin(), add.end());
    sortNotes (l);
}

void deleteSelected (NoteList& l)
{
    l.erase (std::remove_if (l.begin(), l.end(), [] (auto& n) { return n.selected; }), l.end());
}

void muteSelected (NoteList& l, bool m) { forTargets (l, [&] (Note& n) { n.muted = m; }); }
void discardLengths (NoteList& l, double len) { forTargets (l, [&] (Note& n) { n.length = len; }); }
void selectAll (NoteList& l, bool s) { for (auto& n : l) n.selected = s; }
void invertSelection (NoteList& l) { for (auto& n : l) n.selected = ! n.selected; }

void selectRandom (NoteList& l, int seed)
{
    std::mt19937 rng ((unsigned) seed);
    for (auto& n : l) n.selected = (rng() & 1) != 0;
}

void rollStep (NoteList& l, double start, double stepLen, int divisions, int key, float vel)
{
    l.erase (std::remove_if (l.begin(), l.end(), [&] (auto& n)
             { return n.start >= start - 1.0e-6 && n.start < start + stepLen - 1.0e-6; }), l.end());
    divisions = std::max (1, divisions);
    for (int i = 0; i < divisions; ++i)
    {
        Note n; n.key = key; n.start = start + stepLen * i / divisions; n.length = stepLen / divisions;
        n.velocity = vel * (0.7f + 0.3f * (float) (i + 1) / (float) divisions);
        l.push_back (n);
    }
    sortNotes (l);
}
}
