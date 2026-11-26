#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>

// Waveform pyramid (FL / Reaper style): min/max peaks at 256, 1k, 4k, 16k and 64k samples per bin, built once per file on
// one low-priority background thread. Drawing reads only the bins under the visible pixels, so a frame costs the same at
// any zoom and any song length, and nothing is ever re-rendered to zoom.
struct WavePeaks
{
    static constexpr int levels = 5, base = 256;
    double sampleRate = 44100.0; juce::int64 length = 0; int channels = 1;
    std::array<std::vector<juce::int8>, levels> mn, mx;   // [bin * channels + ch], -127..127
    std::atomic<bool> sized { false }, done { false };
    std::atomic<juce::int64> ready { 0 };   // samples analysed so far
    std::atomic<float> peak { 0.0f };
    static juce::int64 binLen (int L) { return (juce::int64) base << (2 * L); }
    juce::int64 bins (int L) const { return (length + binLen (L) - 1) / binLen (L); }
    juce::int64 readyBins (int L) const { return done.load (std::memory_order_acquire) ? bins (L) : ready.load (std::memory_order_acquire) / binLen (L); }

    void build (juce::AudioFormatReader& rd, const std::function<bool()>& stop, std::atomic<juce::uint32>& progress)
    {
        sampleRate = rd.sampleRate > 0 ? rd.sampleRate : 44100.0; length = rd.lengthInSamples;
        channels = juce::jlimit (1, 2, (int) rd.numChannels);
        for (int L = 0; L < levels; ++L) { mn[(size_t) L].assign ((size_t) (bins (L) * channels), 0); mx[(size_t) L].assign ((size_t) (bins (L) * channels), 0); }
        sized.store (true, std::memory_order_release);
        const int chunk = (int) binLen (levels - 1);   // every level's bins end on a chunk edge
        juce::AudioBuffer<float> buf (channels, chunk);
        float pk = 0.0f;
        for (juce::int64 pos = 0; pos < length; pos += chunk)
        {
            if (stop()) return;
            const int n = (int) juce::jmin ((juce::int64) chunk, length - pos);
            rd.read (&buf, 0, n, pos, true, channels > 1);
            const juce::int64 b0 = pos / base, nb = (n + base - 1) / base;
            for (juce::int64 b = 0; b < nb; ++b)
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float* d = buf.getReadPointer (ch) + b * base;
                    const int m = (int) juce::jmin ((juce::int64) base, (juce::int64) n - b * base);
                    float lo = 0.0f, hi = 0.0f;
                    for (int i = 0; i < m; ++i) { lo = std::min (lo, d[i]); hi = std::max (hi, d[i]); }
                    pk = std::max (pk, std::max (hi, -lo));
                    const size_t at = (size_t) ((b0 + b) * channels + ch);
                    mn[0][at] = (juce::int8) juce::jlimit (-127, 127, (int) std::floor (lo * 127.0f));
                    mx[0][at] = (juce::int8) juce::jlimit (-127, 127, (int) std::ceil (hi * 127.0f));
                }
            for (int L = 1; L < levels; ++L)   // fold 4 bins of the level below into one
            {
                const juce::int64 c0 = pos / binLen (L), c1 = (pos + n + binLen (L) - 1) / binLen (L), below = bins (L - 1);
                for (juce::int64 b = c0; b < c1; ++b)
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        int lo = 127, hi = -127;
                        for (juce::int64 j = b * 4; j < std::min (b * 4 + 4, below); ++j)
                        {
                            lo = std::min (lo, (int) mn[(size_t) L - 1][(size_t) (j * channels + ch)]);
                            hi = std::max (hi, (int) mx[(size_t) L - 1][(size_t) (j * channels + ch)]);
                        }
                        mn[(size_t) L][(size_t) (b * channels + ch)] = (juce::int8) lo; mx[(size_t) L][(size_t) (b * channels + ch)] = (juce::int8) hi;
                    }
            }
            peak.store (pk, std::memory_order_relaxed);
            ready.store (pos + n, std::memory_order_release);
            progress.fetch_add (1, std::memory_order_relaxed);
        }
        done.store (true, std::memory_order_release);
        progress.fetch_add (1, std::memory_order_relaxed);
    }

    // channels stacked like AudioThumbnail::drawChannels, one rectangle per pixel column, only the part that's loaded
    void draw (juce::Graphics& g, juce::Rectangle<float> area, double t0, double t1, float gain) const
    {
        if (! sized.load (std::memory_order_acquire) || area.getWidth() < 1.0f || t1 <= t0 || length <= 0) return;
        const int cols = (int) std::ceil (area.getWidth());
        const double spc = (t1 - t0) * sampleRate / area.getWidth();   // samples per pixel
        int L = 0;
        while (L + 1 < levels && (double) binLen (L + 1) <= spc) ++L;
        const juce::int64 bl = binLen (L), rb = readyBins (L);
        const auto& lo8 = mn[(size_t) L]; const auto& hi8 = mx[(size_t) L];
        const float chH = area.getHeight() / (float) channels, k = gain / 127.0f;
        juce::RectangleList<int> rl; rl.ensureStorageAllocated (cols * channels);   // whole-pixel rects: the renderers' fast fill, no anti-aliased edges
        const double s00 = t0 * sampleRate;
        const int x0 = juce::roundToInt (area.getX());
        for (int x = 0; x < cols; ++x)
        {
            const double s0 = s00 + x * spc, s1 = s0 + spc;
            if (s1 <= 0) continue;
            const juce::int64 b0 = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (s0 / (double) bl));
            if (b0 >= rb) break;
            const juce::int64 b1 = juce::jlimit (b0 + 1, rb, (juce::int64) std::ceil (s1 / (double) bl));
            for (int ch = 0; ch < channels; ++ch)
            {
                int lo = 127, hi = -127;
                for (juce::int64 b = b0; b < b1; ++b) { lo = std::min (lo, (int) lo8[(size_t) (b * channels + ch)]); hi = std::max (hi, (int) hi8[(size_t) (b * channels + ch)]); }
                const float mid = area.getY() + chH * ((float) ch + 0.5f), half = chH * 0.5f;
                const float top = mid - juce::jlimit (-1.0f, 1.0f, (float) hi * k) * half, bot = mid - juce::jlimit (-1.0f, 1.0f, (float) lo * k) * half;
                const int y0 = (int) std::floor (top), y1 = juce::jmax (y0 + 1, (int) std::ceil (bot));
                rl.addWithoutMerging ({ x0 + x, y0, 1, y1 - y0 });
            }
        }
        g.fillRectList (rl);
    }
};

// one cache per Playlist; a file is rebuilt when its size or date changes (a re-recorded take with the same name)
struct WaveCache
{
    std::atomic<juce::uint32> progress { 0 };   // bumps as builds advance: the Playlist batches its redraws on this
    WavePeaks* find (const juce::File& f)   // message thread; a stat at most once a second per file, not per frame
    {
        const auto now = juce::Time::getMillisecondCounter();
        auto& e = map[f.getFullPathName()];
        if (e.peaks != nullptr && now - e.checked < 1000) return e.peaks.get();
        e.checked = now;
        const auto size = f.getSize(); const auto date = f.getLastModificationTime().toMilliseconds();
        if (e.peaks != nullptr && size == e.size && date == e.date) return e.peaks.get();
        if (! f.existsAsFile()) return e.peaks.get();
        e.size = size; e.date = date;
        auto w = std::make_shared<WavePeaks>();
        e.peaks = w;
        pool.addJob ([w, f, this] {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (f) })
                w->build (*rd, [this] { return stopping.load(); }, progress);
        });
        return w.get();
    }
    ~WaveCache() { stopping = true; pool.removeAllJobs (true, 4000); }
private:
    std::atomic<bool> stopping { false };
    struct Entry { std::shared_ptr<WavePeaks> peaks; juce::int64 size = -1, date = 0; juce::uint32 checked = 0; };
    std::map<juce::String, Entry> map;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("HHS waveforms").withNumberOfThreads (1).withDesiredThreadPriority (juce::Thread::Priority::low) };
};
