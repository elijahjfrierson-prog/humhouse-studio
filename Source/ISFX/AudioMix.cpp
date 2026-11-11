#include "AudioMix.h"

namespace isfx
{
std::shared_ptr<const std::vector<float>> AudioCache::get (const juce::String& id) const
{
    std::lock_guard<std::mutex> g (lock);
    auto it = data.find (id); return it == data.end() ? nullptr : it->second;
}
void AudioCache::put (const juce::String& id, std::shared_ptr<const std::vector<float>> s)
{
    std::lock_guard<std::mutex> g (lock); data[id] = std::move (s);
}
bool AudioCache::has (const juce::String& id) const
{
    std::lock_guard<std::mutex> g (lock); return data.count (id) > 0;
}

static void mixClip (const Clip& c, const AudioCache& cache, juce::int64 start, int n, float* L, float* R)
{
    const double toSamp = (double) kMixRate / (double) kFlicks;
    juce::int64 c0 = (juce::int64) std::llround ((double) c.start * toSamp), c1 = (juce::int64) std::llround ((double) c.end() * toSamp);
    juce::int64 a = std::max (start, c0), b = std::min (start + n, c1);
    if (a >= b) return;
    auto buf = cache.get (c.mediaId);
    if (! buf) return;
    const juce::int64 frames = (juce::int64) buf->size() / 2;
    const double srcBase = (double) c.srcIn * toSamp;
    // 5 ms fades at clip edges avoid clicks on cuts
    const int fade = kMixRate / 200;
    for (juce::int64 i = a; i < b; ++i)
    {
        double sp = srcBase + (double) (c.reverse ? c1 - 1 - i : i - c0) * c.speed;
        juce::int64 si = (juce::int64) sp;
        if (si < 0 || si + 1 >= frames) continue;
        float fr = (float) (sp - (double) si);
        float g = c.volume;
        if (i - c0 < fade) g *= (float) (i - c0) / fade;
        if (c1 - i < fade) g *= (float) (c1 - i) / fade;
        auto* p = buf->data() + si * 2;
        L[i - start] += g * (p[0] + (p[2] - p[0]) * fr);
        R[i - start] += g * (p[1] + (p[3] - p[1]) * fr);
    }
}

void mixSequence (const Sequence& s, const AudioCache& cache, juce::int64 start, int n, float* L, float* R, TrackFxHost* fx)
{
    std::fill (L, L + n, 0.0f); std::fill (R, R + n, 0.0f);
    bool anySolo = false;
    for (auto& t : s.tracks) anySolo = anySolo || t.solo;
    auto audible = [&] (const Track& tr) { return ! (tr.mute || (anySolo && ! tr.solo)); };
    std::vector<bool> inserts (s.tracks.size(), false);
    if (fx != nullptr)
        for (size_t t = 0; t < s.tracks.size(); ++t)
            inserts[t] = s.tracks[t].id.isNotEmpty() && audible (s.tracks[t]) && fx->has (s.tracks[t].id);
    for (auto& c : s.clips)
    {
        if (c.track < 0 || c.track >= (int) s.tracks.size() || inserts[(size_t) c.track]) continue;
        if (! audible (s.tracks[(size_t) c.track]) || ! c.enabled || c.volume <= 0.0f) continue;
        mixClip (c, cache, start, n, L, R);
    }
    thread_local std::vector<float> tL, tR;
    for (size_t t = 0; t < inserts.size(); ++t)
    {
        if (! inserts[t]) continue;
        tL.assign ((size_t) n, 0.0f); tR.assign ((size_t) n, 0.0f);
        for (auto& c : s.clips)
            if (c.track == (int) t && c.enabled && c.volume > 0.0f) mixClip (c, cache, start, n, tL.data(), tR.data());
        fx->process (s.tracks[t].id, tL.data(), tR.data(), n);   // runs even on silence so reverb/delay tails ring out
        for (int i = 0; i < n; ++i) { L[i] += tL[(size_t) i]; R[i] += tR[(size_t) i]; }
    }
    for (int i = 0; i < n; ++i) { L[i] = juce::jlimit (-1.0f, 1.0f, L[i]); R[i] = juce::jlimit (-1.0f, 1.0f, R[i]); }
}
}
