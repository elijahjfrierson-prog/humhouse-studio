#include "Instruments.h"
#include "Drummer/DrummerCore.h"
#include "DrumSynth.h"
#include <cmath>

namespace hhs
{
static inline double keyToHz (double key) { return 440.0 * std::pow (2.0, (key - 69.0) / 12.0); }

// ------------------------------------------------------------ sampler
class SamplerInstrument : public ChannelInstrument
{
public:
    explicit SamplerInstrument (SharedSample s) : sample (std::move (s)) { sustainPos = findSustain(); findBody(); }
    void prepare (double sr, int) override { sampleRate = sr; }
    void update (const Channel& c) override
    {
        rootKey = c.rootKey; keyTrack = c.keyTrack; pitch = c.stretchMode == 0 ? c.pitch : 0.0f; glide = c.glide; mono = c.mono;
        oneShot = c.oneShot; cutItself = c.cutItself; attack = c.ampAttack; hold = c.ampHold; release = c.ampRelease;
        keepTime = c.stretchMode != 0;
    }
    void noteOn (int key, float vel, bool slide) override
    {
        if (! sample || sample->getNumSamples() == 0) return;
        const double target = (keyTrack ? key - rootKey : 0) + pitch;   // key off: the sample stays the sample
        if (cutItself && ! (glide > 0.0f && (mono || slide))) choke();
        if (! mono && slide && glide > 0.0f)
        {
            // poly portamento / sliding chords (FL): every note of the chord glides the nearest held voice that no
            // other note of this chord took yet - a chord slides as a chord instead of collapsing onto one voice
            Voice* best = nullptr; double bestD = 1.0e9;
            for (auto& v : voices)
                if (v.active && v.claim != clock && (! v.releasing || (v.relAge >= 0 && v.age - v.relAge < 0.06 * sampleRate)))
                    if (const double d = std::abs (v.targetSemis - target); d < bestD) { bestD = d; best = &v; }
            if (best != nullptr)
            {
                auto& v = *best;
                v.releasing = false; v.rel = 1.0f; v.claim = clock;
                v.targetSemis = target; v.key = key; v.holdFrom = v.age;
                v.glideStep = glideStepFor (target - v.semis);
                if (! v.slid) { v.slid = true; v.headA = v.headB = v.pos; v.phase = 0; if (v.pos > sustainPos) v.pos = sustainPos; }
                lastSemis = target;
                return;
            }
        }
        else if (mono || slide)
        {
            // legato: the previous note's note-off lands on the same sample as this note-on, so a voice released
            // right now is still the held note - keep it playing and glide it instead of cutting it off
            for (auto& v : voices) if (v.active && v.releasing && v.relAge >= 0 && v.age - v.relAge < 0.06 * sampleRate && glide > 0.0f) { v.releasing = false; v.rel = 1.0f; }   // FL: a slide/legato note keeps the voice even with a small gap
            for (auto& v : voices) if (v.active && ! v.releasing)
            {
                if (glide > 0.0f)
                {
                    v.targetSemis = target; v.key = key; v.holdFrom = v.age;
                    v.glideStep = glideStepFor (target - v.semis);
                    if (! v.slid)   // a slid note sustains to the end of the note
                    {
                        v.slid = true; v.headA = v.headB = v.pos; v.phase = 0;
                        // a one-shot that already played into its tail goes back to the body: the heads realign there
                        // over the next crossfade, so the slide comes back up instead of gliding on a faded tail
                        if (v.pos > sustainPos) v.pos = sustainPos;
                    }
                    lastSemis = target;
                    return;
                }
                v.releasing = true; v.fadeLen = juce::jmax (v.fadeLen, 0.006f);
            }
        }
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr) for (auto& c : voices) if (c.releasing) { v = &c; break; }   // steal a fading voice before a held one
        if (v == nullptr) { v = &voices[(size_t) next]; next = (next + 1) % (int) voices.size(); }
        *v = {};
        v->active = true; v->dir = 1; v->key = key; v->gain = vel; v->fadeLen = juce::jmax (0.002f, release);
        v->semis = (glide > 0.0f && lastSemis > -900.0 && (mono || slide)) ? lastSemis : target;
        v->targetSemis = target;
        v->glideStep = glideStepFor (target - v->semis);
        if (v->semis != target) v->slid = true;   // a fresh voice that glides in sustains like a legato slide (no dying 808 chains)
        if (! mono && slide) v->claim = clock;
        lastSemis = target;
    }
    // Glide = the time the slide takes, whatever the interval (FL): a 1 s glide reaches the note after 1 s
    double glideStepFor (double semis) const { return glide > 0.0f ? juce::jmax (1.0e-9, std::abs (semis) / (glide * sampleRate)) : 1.0e9; }
    void noteOnAt (int key, float vel, bool slide, double secondsIn) override
    {
        noteOn (key, vel, slide);
        if (secondsIn <= 0.0) return;
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.key == key && v.pos == 0.0)
            {
                v.pos = secondsIn * sampleRate * (keepTime ? 1.0 : std::pow (2.0, v.semis / 12.0));
                v.headA = v.headB = v.pos;
                v.midStart = true;
                if (sample && v.pos >= sample->getNumSamples() - 2) v.active = false;
                break;
            }
    }
    void noteOff (int key) override
    {
        // a slid one-shot is a held note (it parks on its body), so it ends with its note; a chord note that just
        // took this voice can't be ended by the old chord's note-off landing on the same sample
        for (auto& v : voices) if (v.active && ! v.releasing && v.key == key && (mono || ! oneShot || v.slid) && v.claim != clock) { v.releasing = true; v.relAge = v.age; }
    }
    void allNotesOff() override { for (auto& v : voices) v.active = false; }
    void choke() override   // 6 ms: an 808 sub cut any faster clicks; voices already in a long release get cut too
    {
        for (auto& v : voices) if (v.active) { v.fadeLen = v.releasing ? juce::jmin (v.fadeLen, 0.006f) : 0.006f; v.releasing = true; }
    }
    bool isActive() const override { for (auto& v : voices) if (v.active) return true; return false; }

    void render (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! sample) return;
        const int len = sample->getNumSamples();
        const float* l = sample->getReadPointer (0);
        const float* r = sample->getReadPointer (sample->getNumChannels() > 1 ? 1 : 0);
        auto* oL = out.getWritePointer (0);
        auto* oR = out.getWritePointer (1);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            for (int i = start; i < start + num; ++i)
            {
                if (v.semis != v.targetSemis)
                {
                    // smooth ease in and out (not a hard straight ramp): lands on the note exactly at the Glide time
                    const double d = v.targetSemis - v.semis;
                    const double left = std::abs (d) / juce::jmax (1.0e-12, v.glideStep * glide * sampleRate);   // share of the slide still to go
                    const double eased = v.glideStep * juce::jlimit (0.35, 1.8, 1.8 * std::sqrt (left));
                    v.semis = std::abs (d) <= eased ? v.targetSemis : v.semis + (d > 0 ? eased : -eased);
                }
                const int i0 = (int) v.pos;
                if (i0 + 1 >= len) { v.active = false; break; }
                const float f = (float) (v.pos - i0);
                float g = v.gain;
                const double ramp = std::max ((double) attack, v.midStart ? 0.004 : 0.0) * sampleRate;   // hits from the top play exactly like the file; mid-sample starts get 4 ms
                if (v.age < ramp) g *= (float) ((v.age + 1.0) / ramp);
                const double left = (double) (len - 1) - v.pos, tailN = 0.002 * sampleRate;   // samples that end above zero don't click
                if (left < tailN) g *= (float) juce::jmax (0.0, left / tailN);
                if (hold > 0.0f && ! v.releasing && v.age - v.holdFrom > hold * sampleRate) { v.releasing = true; v.relAge = v.age; }
                v.age += 1.0;
                if (v.releasing) { v.rel -= 1.0f / (v.fadeLen * (float) sampleRate); if (v.rel <= 0.0f) { v.active = false; break; } g *= v.rel; }
                auto herm = [&] (const float* x)   // 4-point Hermite: pitched hits stay smooth instead of gritty/blocky
                {
                    const float xm = x[i0 > 0 ? i0 - 1 : 0], x0 = x[i0], x1 = x[i0 + 1], x2 = x[i0 + 2 < len ? i0 + 2 : i0 + 1];
                    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
                    return ((c3 * f + c2) * f + c1) * f + x0;
                };
                const double rate = std::pow (2.0, v.semis / 12.0);
                if (! keepTime && ! v.slid)
                {
                    oL[i] += herm (l) * g;
                    oR[i] += herm (r) * g;
                    v.pos += rate;
                    continue;
                }
                // Stretch: the note plays for the sample's own length at any pitch or glide (FL). Two read heads
                // move at the note's pitch while the time moves at 1x; each restarts every half grain where it
                // lines up with the other one, and they crossfade - so slides fill the note instead of running out.
                const int G = juce::jmax (64, (int) (0.06 * sampleRate)) & ~1;
                const float wA = (float) std::pow (std::sin (juce::MathConstants<double>::pi * v.phase / G), 2.0);
                const float hg = v.slid && bodyLen > 0.0 && v.pos >= sustainPos ? g * bodyGain (v.pos) : g;
                oL[i] += (wA * readAt (l, len, v.headA) + (1.0f - wA) * readAt (l, len, v.headB)) * hg;
                oR[i] += (wA * readAt (r, len, v.headA) + (1.0f - wA) * readAt (r, len, v.headB)) * hg;
                // a slid note keeps sounding for as long as it's held (an 808 slide never "gives up" half way):
                // past the body point the time sweeps slowly back and forth over a long stretch of the body
                // (never one short piece repeated = the stutter), level-matched so the envelope never runs out
                // (the sweep stops short of the end by what the heads read ahead at this pitch)
                const double sweepEnd = juce::jmin (sustainPos + bodyLen, (double) len - 2.0 - G * (rate + 1.0) - 400.0);
                if (v.slid && v.pos >= sustainPos && sweepEnd > sustainPos + 0.02 * sampleRate)
                {
                    v.pos += 0.5 * v.dir;
                    if (v.pos >= sweepEnd) { v.pos = sweepEnd; v.dir = -1; }
                    else if (v.pos <= sustainPos) { v.pos = sustainPos; v.dir = 1; }
                }
                else if (v.slid && ! v.releasing && v.pos >= sustainPos) v.pos = sustainPos;
                else v.pos += 1.0;
                v.headA += rate; v.headB += rate;
                if (++v.phase >= G) { v.phase = 0; v.headA = alignHead (l, len, v.pos, v.headB, rate); }
                else if (v.phase == G / 2) v.headB = alignHead (l, len, v.pos, v.headA, rate);
            }
        }
        clock += num;
    }
private:
    // where a held slide parks: the first point after the peak where the sound has dropped to half (-6 dB),
    // at most half way in - so a long decaying 808 slide keeps its body instead of fading out by the 3rd note
    double findSustain() const
    {
        if (! sample || sample->getNumSamples() < 64) return 0.0;
        const int len = sample->getNumSamples(), win = 480;
        const float* x = sample->getReadPointer (0);
        std::vector<float> rms;
        for (int a = 0; a + win <= len; a += win)
        {
            double sum = 0; for (int i = a; i < a + win; ++i) sum += (double) x[i] * x[i];
            rms.push_back ((float) std::sqrt (sum / win));
        }
        if (rms.empty()) return 0.5 * len;
        const auto peakIt = std::max_element (rms.begin(), rms.end());
        const size_t peak = (size_t) std::distance (rms.begin(), peakIt);
        for (size_t w = peak + 1; w < rms.size(); ++w)
            if (rms[w] < 0.5f * *peakIt) return juce::jmin (0.5 * len, (double) (w * win));
        return 0.5 * len;
    }
    // the stretch of body a held slide sweeps over (up to 0.4 s after the body point, while it's still above
    // -24 dB of it), and the level of the body there so the sweep holds the body point's level the whole time
    void findBody()
    {
        bodyLen = 0.0; bodyRms.clear();
        if (! sample || sample->getNumSamples() < 4096) return;
        const int len = sample->getNumSamples(), s0 = (int) sustainPos;
        const float* x = sample->getReadPointer (0);
        for (int a = s0; a + bodyWin <= len; a += bodyWin)
        {
            double sum = 0; for (int i = a; i < a + bodyWin; ++i) sum += (double) x[i] * x[i];
            bodyRms.push_back ((float) std::sqrt (sum / bodyWin) + 1.0e-9f);
        }
        if (bodyRms.empty()) return;
        size_t n = 1;
        while (n < bodyRms.size() && (double) n * bodyWin < 0.4 * sampleRateHint && bodyRms[n] > bodyRms[0] * 0.063f) ++n;
        bodyLen = (double) (n - 1) * bodyWin;
        if (bodyLen < 0.05 * sampleRateHint) bodyLen = 0.0;   // too short to sweep: parks as before
    }
    float bodyGain (double pos) const
    {
        const double w = (pos - sustainPos) / bodyWin;
        const int k = juce::jlimit (0, (int) bodyRms.size() - 1, (int) w);
        const int k1 = juce::jmin ((int) bodyRms.size() - 1, k + 1);
        const float f = (float) juce::jlimit (0.0, 1.0, w - k), lv = bodyRms[(size_t) k] + f * (bodyRms[(size_t) k1] - bodyRms[(size_t) k]);
        return juce::jlimit (1.0f, 16.0f, bodyRms[0] / lv);
    }
    static float readAt (const float* x, int len, double pos)
    {
        if (pos < 0.0 || pos >= (double) (len - 1)) return 0.0f;
        const int i0 = (int) pos; const float f = (float) (pos - i0);
        const float xm = x[i0 > 0 ? i0 - 1 : 0], x0 = x[i0], x1 = x[i0 + 1], x2 = x[i0 + 2 < len ? i0 + 2 : i0 + 1];
        const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
    // where a restarting head should read from: near the note's time position, on the same cycle as the head
    // that is playing (so the crossfade doesn't comb or flutter); at the sample's own pitch that's exactly `t`
    double alignHead (const float* x, int len, double t, double other, double rate) const
    {
        if (std::abs (rate - 1.0) < 1.0e-6 && std::abs (other - t) < 0.012 * sampleRate) return other;
        const int tol = (int) (0.012 * sampleRate), N = 384;
        const int o = (int) other;
        if (o < 0 || o + N >= len) return t;
        double best = -1.0e30, at = t;
        for (int c = juce::jmax (0, (int) t - tol); c <= juce::jmin (len - N - 1, (int) t + tol); c += 2)
        {
            double xy = 0, yy = 1.0e-9;
            for (int k = 0; k < N; k += 3) { xy += x[c + k] * x[o + k]; yy += x[c + k] * x[c + k]; }
            const double score = xy / std::sqrt (yy) - 1.0e-4 * std::abs (c - t);
            if (score > best) { best = score; at = c; }
        }
        return at;
    }
    struct Voice { bool active = false, releasing = false, midStart = false, slid = false; int key = 0, phase = 0; double pos = 0, semis = 0, targetSemis = 0, glideStep = 1.0e9, age = 0, relAge = -1, holdFrom = 0, headA = 0, headB = 0, claim = -1, dir = 1; float gain = 1, rel = 1, fadeLen = 0.01f; };
    SharedSample sample;
    std::array<Voice, 32> voices {};
    int next = 0, rootKey = 60;
    bool keyTrack = true;
    double sampleRate = 44100.0, lastSemis = -999.0, sustainPos = 0.0, clock = 0.0, bodyLen = 0.0, sampleRateHint = 44100.0;
    static constexpr int bodyWin = 256; std::vector<float> bodyRms;
    float pitch = 0, glide = 0; bool mono = false, keepTime = false;
    bool oneShot = true, cutItself = false; float attack = 0, hold = 0, release = 0.01f;
};

// ------------------------------------------------------------ synth
class SynthInstrument : public ChannelInstrument
{
public:
    void prepare (double sr, int) override { sampleRate = sr; }
    void update (const Channel& c) override
    {
        p = c.synth; glide = c.glide; mono = c.mono; pitch = c.pitch;
        table = p.wave == 10 ? (p.wt != nullptr && p.wt->size() >= (size_t) humWaveFrameSize ? p.wt : humWaveFactory (p.wtKind)) : nullptr;
        for (int o = 0; o < 3; ++o) xRatio[(size_t) o] = std::pow (2.0f, (p.xSemi[(size_t) o] + p.xFine[(size_t) o] / 100.0f) / 12.0f);
    }

    void noteOn (int key, float vel, bool slide) override
    {
        const double k = key + pitch;
        if (! mono && slide && glide > 0.0f)
        {
            // poly portamento / sliding chords: each chord note glides the nearest held voice not taken yet (see sampler)
            Voice* best = nullptr; double bestD = 1.0e9;
            for (auto& v : voices)
                if (v.active && v.claim != clock && (v.stage < 3 || (v.relAge >= 0 && v.age - v.relAge < 0.06 * sampleRate)))
                    if (const double d = std::abs (v.target - k); d < bestD) { bestD = d; best = &v; }
            if (best != nullptr)
            {
                auto& v = *best;
                if (v.stage == 3) v.stage = v.prevStage;
                v.claim = clock; v.target = k; v.key = key; v.velocity = vel; v.glideStep = glideStepFor (k - v.current); v.slid = true; lastKey = k;
                return;
            }
        }
        else if ((mono || slide) && glide > 0.0f)
        {
            for (auto& v : voices) if (v.active && v.stage == 3 && v.relAge >= 0 && v.age - v.relAge < 0.06 * sampleRate) v.stage = v.prevStage;   // legato (see sampler)
            for (auto& v : voices) if (v.active && v.stage < 3)
            { v.target = k; v.key = key; v.velocity = vel; v.glideStep = glideStepFor (k - v.current); v.slid = true; lastKey = k; return; }
        }
        if (mono) for (auto& v : voices) if (v.active) v.stage = 3;
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr) for (auto& c : voices) if (c.stage >= 3) { v = &c; break; }   // steal a releasing voice before a held one
        if (v == nullptr) { v = &voices[(size_t) next]; next = (next + 1) % (int) voices.size(); }
        *v = {};
        v->active = true; v->key = key; v->velocity = vel; v->target = k;
        v->current = (glide > 0.0f && lastKey > 0 && (mono || slide)) ? lastKey : k;
        v->glideStep = glideStepFor (k - v->current);
        v->slid = v->current != k;
        if (! mono && slide) v->claim = clock;
        lastKey = k;
    }
    double clock = 0.0;
    double glideStepFor (double semis) const { return glide > 0.0f ? juce::jmax (1.0e-9, std::abs (semis) / (glide * sampleRate)) : 1.0e9; }
    void noteOff (int key) override { for (auto& v : voices) if (v.active && v.key == key && v.stage < 3 && v.claim != clock) { v.prevStage = v.stage; v.stage = 3; v.relAge = v.age; } }
    void allNotesOff() override { for (auto& v : voices) v.active = false; }
    bool isActive() const override { for (auto& v : voices) if (v.active) return true; return false; }

    void render (juce::AudioBuffer<float>& out, int start, int num) override
    {
        auto* oL = out.getWritePointer (0);
        auto* oR = out.getWritePointer (1);
        clock += num;
        const float sr = (float) sampleRate;
        const float aInc = 1.0f / std::max (1.0f, p.attack * sr);
        const float dCoef = std::exp (-1.0f / std::max (1.0f, p.decay * sr * 0.3f));
        const float rCoef = std::exp (-1.0f / std::max (1.0f, p.release * sr * 0.3f));
        const float swell = 1.0f - std::exp (-1.0f / (0.015f * (float) sr));
        const float g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (20.0f, sr * 0.45f, p.cutoff) / sr);
        const float kq = 2.0f - 2.0f * juce::jlimit (0.0f, 0.97f, p.resonance);
        const float a1 = 1.0f / (1.0f + g * (g + kq)), a2 = g * a1, a3 = g * a2;
        const float detRatio = std::pow (2.0f, p.detune / 1200.0f);

        for (auto& v : voices)
        {
            if (! v.active) continue;
            for (int i = start; i < start + num; ++i)
            {
                if (v.current != v.target)
                {
                    const double d = v.target - v.current;
                    v.current = std::abs (d) <= v.glideStep ? v.target : v.current + (d > 0 ? v.glideStep : -v.glideStep);
                }
                // envelope
                switch (v.stage)
                {
                    case 0: v.env += aInc; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 1; } break;
                    case 1:
                    {
                        // a slid note keeps sounding while it's held (FL 808 slides): the decay parks at half level
                        // instead of dying by the 5th-6th note of a chain, and swells back up if it had faded
                        const float floor = v.slid ? std::max (p.sustain, 0.5f) : p.sustain;
                        if (v.slid && v.env < floor) v.env += (floor - v.env) * swell;
                        else v.env = floor + (v.env - floor) * dCoef;
                        if (p.wave == 4 && v.env < 0.001f) v.active = false;
                        break;
                    }
                    case 2: break;
                    default: v.env *= rCoef; if (v.env < 0.0005f) v.active = false; break;
                }
                if (! v.active) break;
                double hz = keyToHz (v.current);
                if (p.wave == 4) hz *= 1.0 + 1.5 * std::exp (-v.age * 40.0 / sampleRate); // 808 pitch drop
                if (p.vibrato > 0.0f && p.lfoRate > 0.0f)
                    hz *= std::pow (2.0, p.vibrato * std::sin (juce::MathConstants<double>::twoPi * p.lfoRate * v.age / sampleRate) / 12.0);
                const float dt = (float) (hz / sampleRate);
                float s = p.wave >= 5 ? voiceOsc (v, dt) : osc (v.ph1, dt);
                if (p.wave < 5 && (p.detune != 0.0f || p.unison > 1)) s = 0.5f * (s + osc (v.ph2, dt * detRatio));
                if (p.noise > 0.0f) s += p.noise * nextNoise (v);
                v.ph1 += dt; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                v.ph2 += dt * detRatio; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                if (p.drive > 0.0f) s = std::tanh (s * (1.0f + p.drive * 8.0f)) / std::tanh (1.0f + p.drive * 8.0f) ;
                // TPT state-variable lowpass
                const float v3 = s - v.ic2, v1 = a1 * v.ic1 + a2 * v3, v2 = v.ic2 + a2 * v.ic1 + a3 * v3;
                v.ic1 = 2.0f * v1 - v.ic1; v.ic2 = 2.0f * v2 - v.ic2;
                const float y = v2 * v.env * v.velocity * 0.9f;
                oL[i] += y; oR[i] += y;
                ++v.age;
            }
        }
    }
private:
    struct Voice { bool active = false, slid = false; int key = 0, stage = 0, prevStage = 0; double current = 60, target = 60, glideStep = 1.0e9, age = 0, relAge = -1, claim = -1;
                   float env = 0, velocity = 1, ph1 = 0, ph2 = 0.25f, ic1 = 0, ic2 = 0, fmPh = 0;
                   std::array<float, 3> xph { 0.0f, 0.31f, 0.67f };
                   std::array<float, 6> sx { 0.11f, 0.37f, 0.53f, 0.71f, 0.89f, 0.23f };
                   uint32_t rng = 0x9e3779b9u; };
    static float nextNoise (Voice& v)
    {
        v.rng = v.rng * 1664525u + 1013904223u;
        return (float) (v.rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    float voiceOsc (Voice& v, float dt)
    {
        constexpr float tp = juce::MathConstants<float>::twoPi;
        const double t = v.age / sampleRate;
        switch (p.wave)
        {
            case 5: // supersaw: 7 detuned saws
            {
                static constexpr float spread[6] { -1.0f, -0.62f, -0.3f, 0.3f, 0.62f, 1.0f };
                const float amt = 0.004f + p.detune * 0.03f;
                float s = 2.0f * v.ph1 - 1.0f - blep (v.ph1, dt);
                for (size_t k = 0; k < 6; ++k)
                {
                    const float d = dt * (1.0f + spread[k] * amt);
                    float& ph = v.sx[k];
                    s += 0.7f * (2.0f * ph - 1.0f - blep (ph, d));
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                }
                return s * 0.3f;
            }
            case 6: // 2-op FM, index decays like a struck tine / bell
            {
                const float idx = p.fmIndex * (0.25f + 0.75f * (float) std::exp (-t * 2.5));
                const float m = std::sin (tp * v.fmPh);
                v.fmPh += dt * p.fmRatio; v.fmPh -= std::floor (v.fmPh);
                return std::sin (tp * v.ph1 + idx * m);
            }
            case 7: // morphing wavetable swept by the LFO
            {
                const float m = juce::jlimit (0.0f, 1.0f, p.morph + 0.35f * std::sin (tp * (float) (p.lfoRate * t))) * 3.0f;
                const float w[4] { std::sin (tp * v.ph1), 4.0f * std::abs (v.ph1 - 0.5f) - 1.0f,
                                   2.0f * v.ph1 - 1.0f - blep (v.ph1, dt), v.ph1 < 0.5f ? 1.0f : -1.0f };
                const int i0 = juce::jmin (2, (int) m);
                return w[i0] + (w[i0 + 1] - w[i0]) * (m - (float) i0);
            }
            case 9: // 3x Oscillation
            {
                float s = 0.0f;
                for (size_t o = 0; o < 3; ++o)
                {
                    if (p.xVol[o] <= 0.0f) continue;
                    const float d = dt * xRatio[o];
                    float& ph = v.xph[o];
                    float y;
                    switch (p.xWave[o])
                    {
                        case 0: y = 2.0f * ph - 1.0f - blep (ph, d); break;
                        case 1: { y = ph < 0.5f ? 1.0f : -1.0f; y += blep (ph, d); float t2 = ph + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f; y -= blep (t2, d); break; }
                        case 2: y = 4.0f * std::abs (ph - 0.5f) - 1.0f; break;
                        case 3: y = std::sin (tp * ph); break;
                        default: y = nextNoise (v); break;
                    }
                    s += p.xVol[o] * y;
                    ph += d; ph -= std::floor (ph);
                }
                return 0.6f * s;
            }
            case 10: // HumWave wavetable
            {
                if (table == nullptr) return std::sin (tp * v.ph1);
                const auto& t = *table;
                const int frames = (int) t.size() / humWaveFrameSize;
                const float pos = juce::jlimit (0.0f, 1.0f, p.wtPos + (p.lfoRate > 0.0f ? 0.5f * p.morph * std::sin (tp * (float) (p.lfoRate * t0 (v))) : 0.0f));
                const float fp = pos * (float) (frames - 1);
                const int f0 = juce::jmin (frames - 1, (int) fp), f1 = juce::jmin (frames - 1, f0 + 1);
                const float ff = fp - (float) f0;
                auto read = [&] (float ph)
                {
                    const float x = ph * humWaveFrameSize;
                    const int i0 = (int) x & (humWaveFrameSize - 1), i1 = (i0 + 1) & (humWaveFrameSize - 1);
                    const float fx = x - std::floor (x);
                    const float* a = t.data() + (size_t) f0 * humWaveFrameSize;
                    const float* b = t.data() + (size_t) f1 * humWaveFrameSize;
                    const float ya = a[i0] + (a[i1] - a[i0]) * fx, yb = b[i0] + (b[i1] - b[i0]) * fx;
                    return ya + (yb - ya) * ff;
                };
                float s = read (v.ph1);
                if (p.detune > 0.0f || p.unison > 1) s = 0.5f * (s + read (v.ph2));
                return s;
            }
            default: // air: noise over a sine body
                return 0.5f * std::sin (tp * v.ph1) + 0.5f * nextNoise (v);
        }
    }
    double t0 (const Voice& v) const { return v.age / sampleRate; }
    static inline float blep (float t, float dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    inline float osc (float ph, float dt) const
    {
        switch (p.wave)
        {
            case 0: return 2.0f * ph - 1.0f - blep (ph, dt);
            case 1: { float s = ph < 0.5f ? 1.0f : -1.0f; s += blep (ph, dt); float t2 = ph + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f; return s - blep (t2, dt); }
            case 2: return 4.0f * std::abs (ph - 0.5f) - 1.0f;
            default: return std::sin (juce::MathConstants<float>::twoPi * ph);
        }
    }
    SynthParams p;
    std::shared_ptr<const std::vector<float>> table;
    std::array<float, 3> xRatio { 1, 1, 1 };
    std::array<Voice, 24> voices {};
    int next = 0; double sampleRate = 44100.0, lastKey = -1; float glide = 0, pitch = 0; bool mono = false;
};

static void bandLimitFrames (std::vector<float>& t)
{
    constexpr int N = humWaveFrameSize, H = 64;
    const int F = (int) t.size() / N;
    std::vector<float> cs ((size_t) N), sn ((size_t) N);
    std::vector<double> a ((size_t) H + 1), b ((size_t) H + 1);
    for (int f = 0; f < F; ++f)
    {
        float* x = t.data() + (size_t) f * N;
        for (int k = 1; k <= H; ++k)
        {
            double sa = 0, sb = 0;
            for (int i = 0; i < N; ++i)
            {
                const double ph = juce::MathConstants<double>::twoPi * k * i / N;
                sa += x[i] * std::cos (ph); sb += x[i] * std::sin (ph);
            }
            a[(size_t) k] = 2.0 * sa / N; b[(size_t) k] = 2.0 * sb / N;
        }
        float pk = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            double y = 0;
            for (int k = 1; k <= H; ++k)
            {
                const double ph = juce::MathConstants<double>::twoPi * k * i / N;
                y += a[(size_t) k] * std::cos (ph) + b[(size_t) k] * std::sin (ph);
            }
            x[i] = (float) y; pk = std::max (pk, std::abs (x[i]));
        }
        if (pk > 1.0e-6f) for (int i = 0; i < N; ++i) x[i] /= pk;
    }
}

std::shared_ptr<const std::vector<float>> humWaveFromRaw (std::vector<float> t)
{
    t.resize ((size_t) std::max (1, (int) t.size() / humWaveFrameSize) * humWaveFrameSize, 0.0f);
    bandLimitFrames (t);
    return std::make_shared<const std::vector<float>> (std::move (t));
}

std::shared_ptr<const std::vector<float>> humWaveFromAudio (const juce::AudioBuffer<float>& in, int frames)
{
    constexpr int N = humWaveFrameSize, L = 2048;
    const int len = in.getNumSamples();
    std::vector<float> t ((size_t) (frames * N), 0.0f);
    if (len <= 0 || in.getNumChannels() == 0) return humWaveFromRaw (std::move (t));
    // skip the silent lead-in, then spread the frames over the sound
    int first = 0;
    while (first < len - 1 && std::abs (in.getSample (0, first)) < 0.01f) ++first;
    const int span = std::max (1, len - first - L);
    for (int f = 0; f < frames; ++f)
    {
        const int at = first + (frames > 1 ? (int) ((double) span * f / (frames - 1)) : 0);
        for (int i = 0; i < N; ++i)
        {
            double sum = 0; int cnt = 0;
            for (int j = i * L / N; j < (i + 1) * L / N; ++j)
                if (at + j < len) { for (int c = 0; c < in.getNumChannels(); ++c) sum += in.getSample (c, at + j); ++cnt; }
            t[(size_t) (f * N + i)] = cnt > 0 ? (float) (sum / (cnt * in.getNumChannels())) : 0.0f;
        }
    }
    return humWaveFromRaw (std::move (t));
}

const juce::StringArray& humWaveFactoryNames()
{
    static const juce::StringArray n { "Harmonic Sweep", "Pulse Width", "Vowel Morph", "Wave Fold", "Digital Steps", "Sync Sweep", "Organ Draw", "Metal FM" };
    return n;
}

std::shared_ptr<const std::vector<float>> humWaveFactory (int kind)
{
    static const std::vector<std::shared_ptr<const std::vector<float>>> tables = []
    {
        std::vector<std::shared_ptr<const std::vector<float>>> out;
        constexpr int N = humWaveFrameSize, F = 32;
        constexpr double tp = juce::MathConstants<double>::twoPi;
        for (int kd = 0; kd < 8; ++kd)
        {
            std::vector<float> t ((size_t) (F * N));
            for (int f = 0; f < F; ++f)
            {
                const double fr = f / (double) (F - 1);
                for (int i = 0; i < N; ++i)
                {
                    const double p = i / (double) N;
                    double y = 0;
                    switch (kd)
                    {
                        case 0: { const int hm = 1 + (int) (fr * 63); for (int k = 1; k <= hm; ++k) y += std::sin (tp * k * p) / k; break; }
                        case 1: y = p < 0.5 - 0.45 * fr ? 1.0 : -1.0; break;
                        case 2:
                        {
                            const double f1 = 3.0 - 1.6 * fr, f2 = 7.0 + 6.0 * fr;
                            for (int k = 1; k <= 40; ++k)
                                y += (std::exp (-0.5 * std::pow ((k - f1) / 1.2, 2)) + 0.6 * std::exp (-0.5 * std::pow ((k - f2) / 2.0, 2)) + 0.05 / k) * std::sin (tp * k * p);
                            break;
                        }
                        case 3: y = std::sin (std::sin (tp * p) * (1.0 + 7.0 * fr) * juce::MathConstants<double>::halfPi); break;
                        case 4: { const double lv = 32.0 - 29.0 * fr; y = std::floor ((2.0 * p - 1.0) * lv) / lv; break; }
                        case 5: { const double q = p * (1.0 + 7.0 * fr); y = 2.0 * (q - std::floor (q)) - 1.0; y *= 1.0 - p; break; }
                        case 6: { static const double bars[9] { 1, 2, 3, 4, 6, 8, 10, 12, 16 };
                                  for (int k = 0; k < 9; ++k) y += std::sin (tp * bars[k] * p) * (k == 0 ? 1.0 : std::max (0.0, std::sin ((fr * 3.0 + k * 0.7))) * 0.6); break; }
                        default: y = std::sin (tp * p + 6.0 * fr * std::sin (tp * 3.0 * p)); break;
                    }
                    t[(size_t) (f * N + i)] = (float) y;
                }
            }
            bandLimitFrames (t);
            out.push_back (std::make_shared<const std::vector<float>> (std::move (t)));
        }
        return out;
    }();
    return tables[(size_t) juce::jlimit (0, (int) tables.size() - 1, kind)];
}

std::unique_ptr<ChannelInstrument> createInstrument (const Channel& c, SharedSample sample, double sr)
{
    std::unique_ptr<ChannelInstrument> inst;
    if (c.type == ChannelType::Drummer) return createDrummerInstrument (c, sr);
    if (c.type == ChannelType::Synth) inst = std::make_unique<SynthInstrument>();
    else if (c.type == ChannelType::Sampler)
    {
        if (! sample && c.builtinDrum >= 0)
            sample = std::make_shared<const juce::AudioBuffer<float>> (renderBuiltinDrum (c.builtinDrum, sr));
        inst = std::make_unique<SamplerInstrument> (sample);
    }
    if (inst) { inst->prepare (sr, 4096); inst->update (c); }
    return inst;
}

// Band-limited windowed-sinc sample-rate conversion (64 taps, Blackman). Low-passes below the lower
// Nyquist so a 96k/48k file played at 44.1k doesn't fold its highs back down as metallic "notes".
void sincResample (const float* in, int inLen, float* out, int outLen, double ratio)
{
    constexpr int half = 32, res = 512;
    const double fc = juce::jmin (1.0, 1.0 / ratio) * 0.97;
    static thread_local std::vector<float> tab;
    static thread_local double tabFc = -1.0;
    if (tabFc != fc)
    {
        tab.assign ((size_t) (half * res + 2), 0.0f);
        for (int i = 0; i <= half * res; ++i)
        {
            const double x = (double) i / res, px = juce::MathConstants<double>::pi * x;
            const double sinc = i == 0 ? fc : std::sin (px * fc) / px;
            const double w = 0.42 + 0.5 * std::cos (px / half) + 0.08 * std::cos (2.0 * px / half);
            tab[(size_t) i] = (float) (sinc * w);
        }
        tabFc = fc;
    }
    for (int o = 0; o < outLen; ++o)
    {
        const double t = o * ratio;
        const int c = (int) std::floor (t);
        double acc = 0.0, ws = 0.0;
        for (int k = c - half + 1; k <= c + half; ++k)
        {
            const double p = std::abs (t - k) * res;
            const int ip = (int) p;
            if (ip >= half * res) continue;
            const double w = tab[(size_t) ip] + (p - ip) * (tab[(size_t) ip + 1] - tab[(size_t) ip]);
            ws += w;
            if (k >= 0 && k < inLen) acc += w * in[k];
        }
        out[o] = ws != 0.0 ? (float) (acc / ws) : 0.0f;
    }
}

SharedSample loadSampleFile (const juce::File& f, double targetRate)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r || r->lengthInSamples <= 0) return {};
    const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) r->sampleRate * 60);
    juce::AudioBuffer<float> raw (2, len);
    r->read (&raw, 0, len, 0, true, true);
    if (std::abs (r->sampleRate - targetRate) < 1.0) return std::make_shared<const juce::AudioBuffer<float>> (std::move (raw));
    const double ratio = r->sampleRate / targetRate;
    const int outLen = (int) (len / ratio);
    juce::AudioBuffer<float> res (2, outLen);
    for (int ch = 0; ch < 2; ++ch)
        sincResample (raw.getReadPointer (ch), len, res.getWritePointer (ch), outLen, ratio);
    return std::make_shared<const juce::AudioBuffer<float>> (std::move (res));
}

juce::AudioBuffer<float> olaStretch (const juce::AudioBuffer<float>& in, double factor)
{
    const int N = 2048, hopOut = N / 4, inLen = in.getNumSamples(), nch = in.getNumChannels();
    const double hopIn = hopOut / factor;
    const int outLen = juce::jmax (1, (int) (inLen * factor));
    juce::AudioBuffer<float> out (nch, outLen + N); out.clear();
    std::vector<float> win ((size_t) N), norm ((size_t) (outLen + N), 0.0f);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);
    // WSOLA: each grain is taken where its waveform lines up with the one before it (within one hop), so
    // stretched 808s, keys and vocals stay in phase instead of going hollow/flammy; grain 0 starts exactly
    // at the top of the file, so the hit lands on time like FL
    std::vector<float> mono ((size_t) inLen);
    for (int i = 0; i < inLen; ++i) { float m = 0; for (int ch = 0; ch < nch; ++ch) m += in.getSample (ch, i); mono[(size_t) i] = m; }
    const int tol = hopOut, cmpLen = N / 2;
    int prevIp = 0;
    for (int frame = 0;; ++frame)
    {
        const int o = frame * hopOut;
        if (o >= outLen) break;
        int ip = (int) (frame * hopIn);
        if (frame > 0 && inLen > N)
        {
            const int natural = prevIp + hopOut;
            double best = -1.0e30; int bestIp = ip;
            for (int c = std::max (0, ip - tol); c <= std::min (inLen - N, ip + tol); c += 2)
            {
                double acc = 0;
                for (int j = 0; j < cmpLen && natural + j < inLen; j += 2) acc += mono[(size_t) (natural + j)] * mono[(size_t) (c + j)];
                if (acc > best) { best = acc; bestIp = c; }
            }
            ip = bestIp;
        }
        prevIp = ip;
        for (int i = 0; i < N && ip + i < inLen; ++i)
        {
            for (int ch = 0; ch < nch; ++ch) out.addSample (ch, o + i, in.getSample (ch, ip + i) * win[(size_t) i]);
            norm[(size_t) (o + i)] += win[(size_t) i];
        }
    }
    for (int i = 0; i < outLen; ++i)
        if (norm[(size_t) i] > 1.0e-3f) for (int ch = 0; ch < nch; ++ch) out.setSample (ch, i, out.getSample (ch, i) / norm[(size_t) i]);
    out.setSize (nch, outLen, true);
    return out;
}

double sampleTempo (const Channel& c)
{
    if (c.sampleBpm > 0.0f) return c.sampleBpm;
    const auto name = c.sampleFile.getFileNameWithoutExtension().toLowerCase();
    for (int at = name.indexOf ("bpm"); at >= 0; at = name.indexOf (at + 3, "bpm"))
    {
        auto number = [&] (int i, int dir)   // digits right before (dir -1) or after (dir +1) "bpm", skipping spaces/_/-
        {
            while (i >= 0 && i < name.length() && juce::String (" _-").containsChar (name[i])) i += dir;
            int a = i, b = i;
            while (a >= 0 && a < name.length() && juce::CharacterFunctions::isDigit (name[a])) a += dir;
            if (a == b) return 0.0;
            return name.substring (juce::jmin (a, b) + (dir < 0 ? 1 : 0), juce::jmax (a, b) + (dir < 0 ? 1 : 0)).getDoubleValue();
        };
        for (double v : { number (at - 1, -1), number (at + 3, 1) })
            if (v >= 50.0 && v <= 250.0) return v;
    }
    return 0.0;
}

SharedSample processSample (SharedSample src, const Channel& c, double sr, double songBpm)
{
    if (! src || src->getNumSamples() == 0) return src;
    const double own = c.tempoSync && songBpm > 0.0 ? sampleTempo (c) : 0.0;
    const double syncMul = own > 0.0 ? own / songBpm : 1.0;   // 140 BPM loop in a 70 BPM song = twice as long, same pitch
    const bool stretch = (c.stretchMode != 0 && (std::abs (c.stretchTime - 1.0f) > 1.0e-3f || std::abs (c.pitch) > 1.0e-3f))
                      || std::abs (syncMul - 1.0) > 1.0e-4;
    if (! (c.smpNormalize || c.smpReverse || c.smpInvert || c.smpDeclick || c.smpTrim || c.smpBoostDb != 0.0f
           || c.smpFadeIn > 0.0f || c.smpFadeOut > 0.0f || stretch))
        return src;
    juce::AudioBuffer<float> b (*src);
    const int nch = b.getNumChannels();
    if (c.smpTrim)
    {
        const float thr = juce::Decibels::decibelsToGain (-60.0f);
        auto loud = [&] (int i) { for (int ch = 0; ch < nch; ++ch) if (std::abs (b.getSample (ch, i)) > thr) return true; return false; };
        int s0 = 0, s1 = b.getNumSamples() - 1;
        while (s0 < s1 && ! loud (s0)) ++s0;
        while (s1 > s0 && ! loud (s1)) --s1;
        juce::AudioBuffer<float> t (nch, s1 - s0 + 1);
        for (int ch = 0; ch < nch; ++ch) t.copyFrom (ch, 0, b, ch, s0, t.getNumSamples());
        b = std::move (t);
    }
    if (c.smpReverse) b.reverse (0, b.getNumSamples());
    float gain = juce::Decibels::decibelsToGain (c.smpBoostDb) * (c.smpInvert ? -1.0f : 1.0f);
    if (c.smpNormalize)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < nch; ++ch) peak = juce::jmax (peak, b.getMagnitude (ch, 0, b.getNumSamples()));
        if (peak > 1.0e-6f) gain *= 0.98f / peak;
    }
    b.applyGain (gain);
    if (stretch)
    {
        const double ratio = c.stretchMode != 0 ? std::pow (2.0, c.pitch / 12.0) : 1.0;
        const double timeMul = (c.stretchMode != 0 ? (double) c.stretchTime : 1.0) * syncMul;
        auto st = olaStretch (b, juce::jlimit (0.05, 20.0, timeMul * ratio));
        const int outLen = juce::jmax (1, (int) (st.getNumSamples() / ratio));
        juce::AudioBuffer<float> res (nch, outLen);
        for (int ch = 0; ch < nch; ++ch) { juce::LagrangeInterpolator li; li.process (ratio, st.getReadPointer (ch), res.getWritePointer (ch), outLen); }
        b = std::move (res);
    }
    const int n = b.getNumSamples();
    const int fi = juce::jmax (c.smpDeclick ? (int) (0.002 * sr) : 0, (int) (c.smpFadeIn * sr));
    const int fo = juce::jmax (c.smpDeclick ? (int) (0.002 * sr) : 0, (int) (c.smpFadeOut * sr));
    if (fi > 0) b.applyGainRamp (0, juce::jmin (fi, n), 0.0f, 1.0f);
    if (fo > 0) { const int len = juce::jmin (fo, n); b.applyGainRamp (n - len, len, 1.0f, 0.0f); }
    return std::make_shared<const juce::AudioBuffer<float>> (std::move (b));
}
}
