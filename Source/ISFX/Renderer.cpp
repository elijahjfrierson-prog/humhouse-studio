#include "Renderer.h"
#include <cstdio>
#include <array>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>

namespace isfx
{
const std::vector<AspectPreset>& aspectPresets()
{
    static const std::vector<AspectPreset> a {
        { "16:9 Widescreen", 16, 9, "YouTube, TV, desktop" },
        { "9:16 Vertical", 9, 16, "TikTok, Reels, Shorts, Stories" },
        { "1:1 Square", 1, 1, "Instagram / Facebook feed" },
        { "4:5 Portrait", 4, 5, "Instagram portrait feed" },
        { "4:3 Classic TV", 4, 3, "VHS / retro broadcast" },
        { "3:4 Portrait", 3, 4, "Portrait classic" },
        { "2:3 Tall", 2, 3, "Pinterest, posters" },
        { "5:4", 5, 4, "Large-format photo" },
        { "2:1 Univisium", 2, 1, "Streaming cinema" },
        { "1.85:1 Flat", 185, 100, "Cinema flat" },
        { "2.39:1 Scope", 239, 100, "Anamorphic cinema" },
        { "21:9 Ultrawide", 21, 9, "Ultrawide monitors" },
        { "32:9 Super Ultrawide", 32, 9, "LED walls / stage screens" },
    };
    return a;
}

void outputSize (const Project& p, int shortSide, int& W, int& H)
{
    double a = (double) juce::jmax (1, p.aspectW) / juce::jmax (1, p.aspectH);
    if (a >= 1.0) { H = shortSide; W = (int) std::lround (shortSide * a); }
    else          { W = shortSide; H = (int) std::lround (shortSide / a); }
    W += W & 1; H += H & 1;
}

juce::StringArray blendModeNames()
{
    return { "Normal", "Add", "Screen", "Multiply", "Overlay", "Soft Light", "Lighten", "Darken", "Difference", "Color Dodge", "Hard Light", "Exclusion" };
}

// ---------------------------------------------------------------- helpers
namespace
{
class RowPool
{
public:
    static RowPool& get() { static RowPool p; return p; }
    void run (int H, const std::function<void (int)>& fn)
    {
        std::unique_lock<std::mutex> callLock (callMx);
        {
            std::lock_guard<std::mutex> g (mx);
            job = &fn; rows = H; next = 0; done = 0; ++gen;
        }
        cv.notify_all();
        work (gen);
        std::unique_lock<std::mutex> g (mx);
        doneCv.wait (g, [this] { return done >= rows; });
        job = nullptr;
    }
    ~RowPool()
    {
        { std::lock_guard<std::mutex> g (mx); quit = true; ++gen; }
        cv.notify_all();
        for (auto& t : threads) t.join();
    }
private:
    RowPool()
    {
        int n = (int) juce::jlimit (1u, 16u, std::thread::hardware_concurrency()) - 1;
        for (int i = 0; i < n; ++i)
            threads.emplace_back ([this]
            {
                juce::uint64 seen = 0;
                for (;;)
                {
                    juce::uint64 g;
                    {
                        std::unique_lock<std::mutex> l (mx);
                        cv.wait (l, [&] { return quit || gen != seen; });
                        if (quit) return;
                        seen = g = gen;
                    }
                    work (g);
                }
            });
    }
    void work (juce::uint64 g)
    {
        constexpr int band = 8;
        for (;;)
        {
            const std::function<void (int)>* f; int y0, y1;
            {
                std::lock_guard<std::mutex> l (mx);
                if (gen != g || job == nullptr || next >= rows) return;
                f = job; y0 = next; y1 = std::min (rows, next + band); next = y1;
            }
            for (int y = y0; y < y1; ++y) (*f) (y);
            {
                std::lock_guard<std::mutex> l (mx);
                done += y1 - y0;
                if (done >= rows) doneCv.notify_all();
            }
        }
    }
    std::mutex callMx, mx;
    std::condition_variable cv, doneCv;
    std::vector<std::thread> threads;
    const std::function<void (int)>* job = nullptr;
    int rows = 0, next = 0, done = 0;
    juce::uint64 gen = 0;
    bool quit = false;
};

template <typename F> void parallelRows (int H, F&& fn)
{
    if (H < 32) { for (int y = 0; y < H; ++y) fn (y); return; }
    std::function<void (int)> f = [&fn] (int y) { fn (y); };
    RowPool::get().run (H, f);
}

inline float hash3 (int x, int y, int z)
{
    juce::uint32 h = (juce::uint32) x * 374761393u + (juce::uint32) y * 668265263u + (juce::uint32) z * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float) (h & 0xffffff) / 16777215.0f;
}
inline float clamp01 (float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smooth (float a, float b, float x) { float t = clamp01 ((x - a) / (b - a)); return t * t * (3 - 2 * t); }
inline float luma (const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
inline float mixf (float a, float b, float t) { return a + (b - a) * t; }

struct BeatEnv { float kick = 0, snare = 0, snareFast = 0, hat = 0; int kickIdx = 0, snareIdx = -1; double sinceSnare = 1e9; };

BeatEnv beatEnvAt (const SongAnalysis& a, Ticks t)
{
    BeatEnv b;
    if (! a.valid || a.events.empty()) return b;
    auto it = std::upper_bound (a.events.begin(), a.events.end(), t, [] (Ticks v, const MusicEvent& e) { return v < e.time; });
    bool gk = false, gs = false, gh = false;
    while (it != a.events.begin())
    {
        --it;
        double dt = ticksToSec (t - it->time);
        if (dt > 1.0) break;
        int idx = (int) (it - a.events.begin());
        if (it->type == EventType::Kick && ! gk) { gk = true; b.kick = (float) std::exp (-dt / 0.12); b.kickIdx = idx; }
        if (it->type == EventType::Snare && ! gs) { gs = true; b.snare = (float) std::exp (-dt / 0.15); b.snareFast = (float) std::exp (-dt / 0.06); b.snareIdx = idx; b.sinceSnare = dt; }
        if (it->type == EventType::HiHat && ! gh) { gh = true; b.hat = (float) std::exp (-dt / 0.04); }
        if (gk && gs && gh) break;
    }
    return b;
}

// bilinear RGBA sample in rotated-frame pixel space; returns false outside
bool sampleFrame (const Frame& f, int rot, float u, float v, float* out)
{
    float x, y;
    if (rot == 90)       { x = v; y = (float) f.h - 1 - u; }
    else if (rot == 180) { x = (float) f.w - 1 - u; y = (float) f.h - 1 - v; }
    else if (rot == 270) { x = (float) f.w - 1 - v; y = u; }
    else                 { x = u; y = v; }
    if (x < -0.5f || y < -0.5f || x > f.w - 0.5f || y > f.h - 0.5f) return false;
    x = juce::jlimit (0.0f, (float) f.w - 1.001f, x); y = juce::jlimit (0.0f, (float) f.h - 1.001f, y);
    int x0 = (int) x, y0 = (int) y; float fx = x - x0, fy = y - y0;
    int x1 = std::min (x0 + 1, f.w - 1), y1 = std::min (y0 + 1, f.h - 1);
    const juce::uint8* d = f.rgba.data();
    auto P = [&] (int xx, int yy, int c) { return d[((size_t) yy * f.w + xx) * 4 + c] * (1.0f / 255.0f); };
    for (int c = 0; c < 4; ++c)
        out[c] = mixf (mixf (P (x0, y0, c), P (x1, y0, c), fx), mixf (P (x0, y1, c), P (x1, y1, c), fx), fy);
    return true;
}

void sampleBuf (const std::vector<float>& b, int w, int h, float x, float y, float* out)
{
    x = juce::jlimit (0.0f, (float) w - 1.001f, x); y = juce::jlimit (0.0f, (float) h - 1.001f, y);
    int x0 = (int) x, y0 = (int) y; float fx = x - x0, fy = y - y0;
    int x1 = std::min (x0 + 1, w - 1), y1 = std::min (y0 + 1, h - 1);
    for (int c = 0; c < 3; ++c)
    {
        auto P = [&] (int xx, int yy) { return b[((size_t) yy * w + xx) * 3 + c]; };
        out[c] = mixf (mixf (P (x0, y0), P (x1, y0), fx), mixf (P (x0, y1), P (x1, y1), fx), fy);
    }
}

void boxBlur (std::vector<float>& b, int w, int h, int rx, int ry, int passes)
{
    std::vector<float> tmp (b.size());
    for (int p = 0; p < passes; ++p)
    {
        if (rx > 0)
            parallelRows (h, [&] (int y) {
                for (int c = 0; c < 3; ++c)
                {
                    double acc = 0; int n = 0;
                    for (int x = -rx; x <= rx; ++x) if (x >= 0 && x < w) { acc += b[((size_t) y * w + x) * 3 + c]; ++n; }
                    for (int x = 0; x < w; ++x)
                    {
                        tmp[((size_t) y * w + x) * 3 + c] = (float) (acc / std::max (1, n));
                        int xo = x - rx, xi = x + rx + 1;
                        if (xo >= 0) { acc -= b[((size_t) y * w + xo) * 3 + c]; --n; }
                        if (xi < w) { acc += b[((size_t) y * w + xi) * 3 + c]; ++n; }
                    }
                }
            });
        else tmp = b;
        if (ry > 0)
            parallelRows (w, [&] (int x) {
                for (int c = 0; c < 3; ++c)
                {
                    double acc = 0; int n = 0;
                    for (int y = -ry; y <= ry; ++y) if (y >= 0 && y < h) { acc += tmp[((size_t) y * w + x) * 3 + c]; ++n; }
                    for (int y = 0; y < h; ++y)
                    {
                        b[((size_t) y * w + x) * 3 + c] = (float) (acc / std::max (1, n));
                        int yo = y - ry, yi = y + ry + 1;
                        if (yo >= 0) { acc -= tmp[((size_t) yo * w + x) * 3 + c]; --n; }
                        if (yi < h) { acc += tmp[((size_t) yi * w + x) * 3 + c]; ++n; }
                    }
                }
            });
        else b = tmp;
    }
}

void blendPixel (int mode, const float* d, const float* s, float* o)
{
    for (int c = 0; c < 3; ++c)
    {
        float b = d[c], a = s[c], r;
        switch (mode)
        {
            case 1:  r = std::min (1.0f, a + b); break;
            case 2:  r = 1 - (1 - a) * (1 - b); break;
            case 3:  r = a * b; break;
            case 4:  r = b < 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b); break;
            case 5:  r = a < 0.5f ? b - (1 - 2 * a) * b * (1 - b) : b + (2 * a - 1) * ((b < 0.25f ? ((16 * b - 12) * b + 4) * b : std::sqrt (b)) - b); break;
            case 6:  r = std::max (a, b); break;
            case 7:  r = std::min (a, b); break;
            case 8:  r = std::abs (a - b); break;
            case 9:  r = a >= 1 ? 1 : std::min (1.0f, b / (1 - a)); break;
            case 10: r = a < 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b); break;
            case 11: r = a + b - 2 * a * b; break;
            default: r = a; break;
        }
        o[c] = r;
    }
}

void rgbToHsv (const float* c, float& h, float& s, float& v)
{
    float mx = std::max ({ c[0], c[1], c[2] }), mn = std::min ({ c[0], c[1], c[2] }), d = mx - mn;
    v = mx; s = mx > 0 ? d / mx : 0; h = 0;
    if (d > 1e-6f)
    {
        if (mx == c[0]) h = 60 * std::fmod ((c[1] - c[2]) / d + 6, 6.0f);
        else if (mx == c[1]) h = 60 * ((c[2] - c[0]) / d + 2);
        else h = 60 * ((c[0] - c[1]) / d + 4);
    }
}

void thermalColor (float l, float* o)
{
    static const float P[7][3] { { 0, 0, 0 }, { 0.1f, 0, 0.55f }, { 0.65f, 0, 0.6f }, { 1, 0.05f, 0.1f }, { 1, 0.55f, 0 }, { 1, 0.95f, 0.1f }, { 1, 1, 1 } };
    float x = clamp01 (l) * 6; int i = std::min (5, (int) x); float f = x - i;
    for (int c = 0; c < 3; ++c) o[c] = mixf (P[i][c], P[i + 1][c], f);
}
}

namespace
{
struct ToneCurve
{
    bool active = false;
    std::array<float, 4 * 257> t {};  // master, R, G, B
};

ToneCurve makeCurve (const FxStack& fx, const std::vector<ParamDef>& defs)
{
    ToneCurve tc;
    const char chans[4] = { 'M', 'R', 'G', 'B' };
    for (int c = 0; c < 4; ++c)
    {
        float y[5];
        for (int i = 0; i < 5; ++i)
        {
            char id[8]; std::snprintf (id, sizeof (id), "crv%c%d", chans[c], i);
            y[i] = fxValue (fx, defs, id);
            if (std::abs (y[i] - i / 4.0f) > 1e-4f) tc.active = true;
        }
        for (int k = 0; k <= 256; ++k)
        {
            float x = k / 256.0f * 4.0f; int i = std::min (3, (int) x); float u = x - i;
            float p1 = y[i], p2 = y[i + 1];
            float p0 = i > 0 ? y[i - 1] : 2 * p1 - p2, p3 = i < 3 ? y[i + 2] : 2 * p2 - p1;
            float v = 0.5f * (2 * p1 + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (-p0 + 3 * p1 - 3 * p2 + p3) * u * u * u);
            tc.t[(size_t) c * 257 + (size_t) k] = std::clamp (v, 0.0f, 1.0f);
        }
    }
    return tc;
}

inline float curveAt (const float* t, float v)
{
    v = std::clamp (v, 0.0f, 1.0f) * 256.0f;
    int i = std::min (255, (int) v); float u = v - i;
    return t[i] + (t[i + 1] - t[i]) * u;
}

inline void applyCurve (const ToneCurve& tc, float* c)
{
    for (int q = 0; q < 3; ++q) c[q] = curveAt (tc.t.data(), curveAt (tc.t.data() + (q + 1) * 257, c[q]));
}

inline void adjustTones (float* c, float bright, float hi, float sh)
{
    if (bright != 0)
    {
        float g = std::exp (-bright * 0.9f);
        for (int q = 0; q < 3; ++q) c[q] = std::pow (std::clamp (c[q], 0.0f, 1.0f), g);
    }
    if (hi != 0 || sh != 0)
    {
        float l = std::clamp (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], 0.0f, 1.0f);
        float ws = 1 - std::clamp (l / 0.55f, 0.0f, 1.0f), wh = std::clamp ((l - 0.45f) / 0.55f, 0.0f, 1.0f);
        float d = sh * 0.35f * ws * ws * (3 - 2 * ws) + hi * 0.35f * wh * wh * (3 - 2 * wh);
        for (int q = 0; q < 3; ++q) c[q] += d;
    }
}
}

// ---------------------------------------------------------------- sources
void applyLayout (const Project& p, Clip& c, int slot)
{
    static const float r[][4] { { 0, 0, 1, 1 }, { 0, 0, 1, .5f }, { 0, .5f, 1, .5f }, { 0, 0, .5f, 1 }, { .5f, 0, .5f, 1 },
                                { 0, 0, 1, 1 / 3.f }, { 0, 1 / 3.f, 1, 1 / 3.f }, { 0, 2 / 3.f, 1, 1 / 3.f },
                                { 0, 0, .5f, .5f }, { .5f, 0, .5f, .5f }, { 0, .5f, .5f, .5f }, { .5f, .5f, .5f, .5f } };
    slot = juce::jlimit (0, 11, slot);
    for (auto* id : { "scale", "scaleX", "scaleY", "posX", "posY", "rotation", "cropL", "cropR", "cropT", "cropB", "maskShape", "maskW", "maskH", "maskFeather", "maskInvert" })
        c.fx.p.erase (id);
    if (slot == 0) { c.fx.set ("reframe", 0); return; }
    float x = r[slot][0], y = r[slot][1], w = r[slot][2], h = r[slot][3];
    int W = 1920, H = 1080; outputSize (p, 1080, W, H);
    float mw = 16, mh = 9;
    if (auto* m = p.findMedia (c.mediaId); m != nullptr && m->width > 0 && m->height > 0)
    {
        bool sw = m->rotation == 90 || m->rotation == 270;
        mw = (float) (sw ? m->height : m->width); mh = (float) (sw ? m->width : m->height);
    }
    // fill mode scales the source to cover the frame; shrink it so it just covers its slot, then mask to the slot
    float base = std::max ((float) W / mw, (float) H / mh), need = std::max (w * W / mw, h * H / mh);
    c.fx.set ("reframe", 0);
    c.fx.set ("scale", juce::jlimit (0.1f, 4.0f, need / base));
    c.fx.set ("posX", (x + w * 0.5f - 0.5f) * 2); c.fx.set ("posY", (y + h * 0.5f - 0.5f) * 2);
    c.fx.set ("maskShape", 2); c.fx.set ("maskW", w); c.fx.set ("maskH", h); c.fx.set ("maskFeather", 0.0f);
}

bool clipGeometry (const Project& p, const Clip& c, float W, float H, ClipGeom& g)
{
    auto* m = p.findMedia (c.mediaId);
    if (m == nullptr || m->kind == MediaKind::Audio || m->width <= 0 || m->height <= 0 || W <= 0 || H <= 0) return false;
    const auto& CP = clipParams();
    auto cv = [&] (const char* id) { return fxValue (c.fx, CP, id); };
    const bool sw = m->rotation == 90 || m->rotation == 270;
    const float rw = (float) (sw ? m->height : m->width), rh = (float) (sw ? m->width : m->height);
    const int reframe = (int) std::lround (cv ("reframe"));
    const float base = reframe == 0 ? std::max (W / rw, H / rh) : std::min (W / rw, H / rh);
    const float s = base * cv ("scale");
    const float rot = -cv ("rotation") * juce::MathConstants<float>::pi / 180.0f;
    g.cr = std::cos (rot); g.sr = std::sin (rot);
    g.cx = W * 0.5f + cv ("posX") * W * 0.5f; g.cy = H * 0.5f + cv ("posY") * H * 0.5f;
    g.hx = rw * 0.5f * s * cv ("scaleX"); g.hy = rh * 0.5f * s * cv ("scaleY");
    float cL = cv ("cropL"), cR = cv ("cropR");
    if (cv ("mirror") > 0.5f) std::swap (cL, cR);
    g.x0 = -g.hx + 2 * g.hx * cL; g.x1 = g.hx - 2 * g.hx * cR;
    g.y0 = -g.hy + 2 * g.hy * cv ("cropT"); g.y1 = g.hy - 2 * g.hy * cv ("cropB");
    return true;
}

juce::StringArray layoutNames()
{
    return { "Full frame", "Top half", "Bottom half", "Left half", "Right half", "Top third", "Middle third", "Bottom third",
             "Grid top-left", "Grid top-right", "Grid bottom-left", "Grid bottom-right" };
}

void Renderer::clearSources() { sources.clear(); luts.clear(); trailT = -1; }

const Frame* Renderer::frameFor (const Project& p, const juce::String& mediaId, Ticks srcT, int decodeH, int& rotation, bool* opaque, bool primeOnly)
{
    auto* m = p.findMedia (mediaId);
    if (m == nullptr || m->missing || m->kind == MediaKind::Audio) return nullptr;
    const bool useProxy = m->kind == MediaKind::Video && decodeH > 0 && decodeH <= kProxyHeight
                          && m->proxyPath.isNotEmpty() && juce::File (m->proxyPath).existsAsFile();
    const bool async = live && m->kind == MediaKind::Video;
    auto key = mediaId + "@" + juce::String (decodeH) + (useProxy ? "p" : "") + (async ? "L" : "");
    // Free decoders that haven't been on screen for a while (RAM), and keep the open count bounded. Never evict a
    // source used during this render: its frame may still be waiting to be composited.
    if ((useClock & 63) == 0)
        for (auto it = sources.begin(); it != sources.end();)
            it = (it->second->used < renderStart && useClock - it->second->used > 900) ? sources.erase (it) : std::next (it);
    if (sources.size() >= kMaxOpenSources && sources.find (key) == sources.end())
    {
        auto oldest = sources.begin();
        for (auto it = sources.begin(); it != sources.end(); ++it) if (it->second->used < oldest->second->used) oldest = it;
        if (oldest->second->used < renderStart) sources.erase (oldest);
    }
    auto& src = sources[key];
    if (src == nullptr)
    {
        src = std::make_unique<Source>();
        src->used = ++useClock;
        juce::File f (m->path);
        if (m->kind == MediaKind::Image)
        {
            auto img = juce::ImageFileFormat::loadFrom (f);
            if (img.isValid() && decodeH > 0 && img.getHeight() > decodeH)
                img = img.rescaled (std::max (1, img.getWidth() * decodeH / img.getHeight()), decodeH, juce::Graphics::highResamplingQuality);
            if (img.isValid())
            {
                img = img.convertedToFormat (juce::Image::ARGB);
                juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
                src->still.w = img.getWidth(); src->still.h = img.getHeight(); src->still.pts = 0;
                src->still.rgba.resize ((size_t) src->still.w * src->still.h * 4);
                for (int y = 0; y < src->still.h; ++y)
                    for (int x = 0; x < src->still.w; ++x)
                    {
                        auto c = bd.getPixelColour (x, y);
                        auto* o = &src->still.rgba[((size_t) y * src->still.w + x) * 4];
                        o[0] = c.getRed(); o[1] = c.getGreen(); o[2] = c.getBlue(); o[3] = c.getAlpha();
                    }
            }
            else src->failed = true;
        }
        else if (async)
        {
            src->pre = std::make_unique<PrefetchDecoder>();
            src->fps = m->fps > 0 ? m->fps : 30.0;
            juce::String err;
            if (useProxy && src->pre->open (juce::File (m->proxyPath), decodeH, err)) src->rotation = m->rotation;
            else
            {
                src->pre = std::make_unique<PrefetchDecoder>();
                if (! src->pre->open (f, decodeH, err)) src->failed = true;
                else src->rotation = src->pre->rotation();
            }
        }
        else
        {
            src->dec = std::make_unique<VideoDecoder>();
            juce::String err;
            if (useProxy && src->dec->open (juce::File (m->proxyPath), decodeH, err)) src->rotation = m->rotation;
            else if (! src->dec->open (f, decodeH, err)) src->failed = true;
            else src->rotation = src->dec->rotation();
        }
    }
    if (src->failed) return nullptr;
    src->used = ++useClock;
    rotation = src->rotation;
    if (m->kind == MediaKind::Image)
    {
        if (src->opaque < 0)
        {
            src->opaque = 1;
            for (size_t i = 3; i < src->still.rgba.size(); i += 4) if (src->still.rgba[i] < 255) { src->opaque = 0; break; }
        }
        if (opaque) *opaque = src->opaque == 1;
        return &src->still;
    }
    if (opaque) *opaque = true;
    if (src->pre != nullptr)
    {
        if (primeOnly) { src->pre->prime (srcT); return nullptr; }
        // only the very first frame of a source may wait (briefly); after that a late frame repeats the last one
        if (auto f = src->pre->get (juce::jmax ((Ticks) 0, srcT), src->shown ? 0 : 80)) src->shown = std::move (f);
        if (opaque == nullptr && src->shown != nullptr && srcT - src->shown->pts > secToTicks (2.5 / src->fps)) ++late;
        return src->shown.get();
    }
    if (primeOnly) return nullptr;
    if (auto* fr = src->dec->frameRef (juce::jmax ((Ticks) 0, srcT))) return fr;
    return nullptr;
}

std::shared_ptr<Lut3D> Renderer::lutFor (const juce::String& path)
{
    if (path.isEmpty()) return nullptr;
    auto it = luts.find (path);
    if (it != luts.end()) return it->second;
    auto l = std::make_shared<Lut3D>();
    juce::String err;
    if (! l->load (juce::File (path), err)) l = nullptr;
    luts[path] = l;
    return l;
}

// ---------------------------------------------------------------- render
bool Renderer::render (const Project& p, const Sequence& s, Ticks t, int W, int H, std::vector<juce::uint8>& rgba, int decodeH)
{
    renderStart = ++useClock;
    if (W <= 0 || H <= 0) return false;
    const size_t NP = (size_t) W * H;
    std::vector<float> img (NP * 3, 0.0f);
    const double T = ticksToSec (t);
    const int frameIdx = (int) ticksToFrame (t, p.fps);
    const auto& SP = sequenceParams();
    // effect clips (no media) on visible layers blend their look into the sequence FX while the playhead is over them
    FxStack postFx = s.fx;
    for (auto& c : s.clips)
        if (c.mediaId.isEmpty() && c.enabled && t >= c.start && t < c.end() && c.track >= 0 && c.track < (int) s.tracks.size()
            && ! s.tracks[(size_t) c.track].hidden)
        {
            for (auto& [k, v] : c.fx.p)
            {
                auto* d = findParam (SP, k);
                float base = postFx.get (k, d ? d->def : 0.0f);
                postFx.set (k, base + (v - base) * c.opacity);
            }
            if (c.fx.lutPath.isNotEmpty()) postFx.lutPath = c.fx.lutPath;
        }
    auto sv = [&] (const char* id) { return fxValue (postFx, SP, id); };
    const BeatEnv be = beatEnvAt (p.song, t);
    const float minWH = (float) std::min (W, H);

    // global "camera": kick zoom + shake
    float camZoom = 1.0f + sv ("kickZoom") * 0.07f * be.kick;
    float shakeAmt = sv ("shake") * be.kick;
    float camX = (hash3 (be.kickIdx, 1, 3) - 0.5f) * 0.035f * W * shakeAmt;
    float camY = (hash3 (be.kickIdx, 2, 5) - 0.5f) * 0.035f * H * shakeAmt;
    float transGlitch = 0, transSplit = 0, transBlur = 0;
    // per-layer transition state, set just before drawLayer and reset after
    float trRot = 0, trStretch = 1, trShX = 0, trShY = 0, trMirror = 0, trAmt = 1, trPrism = 0, trLeak = 0;
    int trWipe = 0, trDir = 0;

    // ---- composite video tracks (V1 bottom -> top)
    std::vector<const Clip*> active;
    for (auto& c : s.clips)
    {
        if (c.track < 0 || c.track >= (int) s.tracks.size()) continue;
        auto& tr = s.tracks[(size_t) c.track];
        if (tr.kind != TrackKind::Video || tr.hidden || ! c.enabled) continue;
        if (t >= c.start && t < c.end()) active.push_back (&c);
    }
    std::stable_sort (active.begin(), active.end(), [] (const Clip* a, const Clip* b) { return a->track < b->track; });
    if (live)
    {
        // pre-roll: open + seek the next clips ~1.5 s before their cut, so the cut itself costs nothing
        std::vector<const Clip*> soon;
        for (auto& c : s.clips)
            if (c.enabled && c.start > t && c.start - t <= secToTicks (1.5) && c.track >= 0 && c.track < (int) s.tracks.size()
                && s.tracks[(size_t) c.track].kind == TrackKind::Video && ! s.tracks[(size_t) c.track].hidden && c.mediaId.isNotEmpty())
                soon.push_back (&c);
        std::sort (soon.begin(), soon.end(), [] (const Clip* a, const Clip* b) { return a->start < b->start; });
        int rr = 0;
        for (size_t i = 0; i < soon.size() && i < 3; ++i) frameFor (p, soon[i]->mediaId, soon[i]->srcAt (soon[i]->start), decodeH, rr, nullptr, true);
    }
    {
        // a full-frame opaque layer hides everything under it: skip decoding/drawing those (keeps deep stacks cheap)
        const auto& CPo = clipParams();
        auto covers = [&] (const Clip& c)
        {
            auto* m = p.findMedia (c.mediaId);
            if (m == nullptr || m->missing || m->kind == MediaKind::Audio || c.opacity < 1.0f) return false;
            if (c.transIn != TransitionType::Cut && c.transLen > 0 && t < c.start + c.transLen) return false;
            auto v = [&] (const char* id) { return fxValue (c.fx, CPo, id); };
            int rf = (int) std::lround (v ("reframe"));
            return (rf == 0 || rf == 2) && v ("blend") < 0.5f && v ("chromaKey") <= 0 && v ("lumaKey") <= 0 && v ("maskShape") < 0.5f
                && v ("scale") >= 1 && v ("scaleX") >= 1 && v ("scaleY") >= 1 && v ("posX") == 0 && v ("posY") == 0 && v ("rotation") == 0
                && v ("cropL") <= 0 && v ("cropR") <= 0 && v ("cropT") <= 0 && v ("cropB") <= 0
                && [&] { int rr = 0; bool op = false; return frameFor (p, c.mediaId, c.srcAt (t), decodeH, rr, &op) != nullptr && op; }();
        };
        for (int i = (int) active.size() - 1; i > 0; --i)
            if (covers (*active[(size_t) i])) { active.erase (active.begin(), active.begin() + i); break; }
    }

    auto drawLayer = [&] (const Clip& c, float alphaMul, float extraScale, float whip, int transKind, float prog)
    {
        int rot = 0;
        const Frame* fr = frameFor (p, c.mediaId, c.srcAt (t), decodeH, rot);
        if (fr == nullptr) return;
        const auto& CP = clipParams();
        auto cv = [&] (const char* id) { return fxValue (c.fx, CP, id); };
        float rw = (float) (rot == 90 || rot == 270 ? fr->h : fr->w), rh = (float) (rot == 90 || rot == 270 ? fr->w : fr->h);
        int reframe = (int) std::lround (cv ("reframe"));
        float sFill = std::max (W / rw, H / rh), sFit = std::min (W / rw, H / rh);
        float base = reframe == 0 ? sFill : sFit;
        float scale = base * cv ("scale") * camZoom * extraScale;
        const float sclX = scale * cv ("scaleX"), sclY = scale * cv ("scaleY");
        const float cL = cv ("cropL"), cR = cv ("cropR"), cT = cv ("cropT"), cB = cv ("cropB");
        const bool cropped = cL > 0 || cR > 0 || cT > 0 || cB > 0;
        float rotRad = -cv ("rotation") * juce::MathConstants<float>::pi / 180.0f + trRot;
        float cr = std::cos (rotRad), sr = std::sin (rotRad);
        bool mirror = cv ("mirror") > 0.5f;
        float offX = cv ("posX") * W * 0.5f + camX + (trDir < 2 ? whip * W : 0.0f) + trShX * W;
        float offY = cv ("posY") * H * 0.5f + camY + (trDir >= 2 ? whip * H : 0.0f) + trShY * H;
        float cExp = std::pow (2.0f, cv ("exposure")), cCon = cv ("contrast"), cSat = cv ("saturation");
        const float cBri = cv ("brightness"), cHi = cv ("highlights"), cSh = cv ("shadows"), cTemp = cv ("temperature"), cTint = cv ("tint");
        const ToneCurve cCurve = makeCurve (c.fx, CP);
        int blend = juce::jlimit (0, 11, (int) std::lround (cv ("blend")));
        float chroma = cv ("chromaKey"), keyHue = cv ("keyHue"), keyTol = cv ("keyTol");
        float lumaK = cv ("lumaKey"), lumaT = cv ("lumaThresh");
        int maskShape = (int) std::lround (cv ("maskShape"));
        float mW = std::max (0.01f, cv ("maskW")), mH = std::max (0.01f, cv ("maskH")), mF = cv ("maskFeather");
        bool mInv = cv ("maskInvert") > 0.5f;
        float alpha0 = c.opacity * alphaMul;
        int taps = whip != 0 ? 6 : 1;
        float tapLen = (1 - prog) * W * 0.06f;

        // blur-fill background: fill-scaled copy, heavily blurred and darkened
        std::vector<float> bg; int bw = 0, bh = 0;
        if (reframe == 2)
        {
            bw = std::max (8, W / 8); bh = std::max (8, H / 8);
            bg.assign ((size_t) bw * bh * 3, 0.0f);
            float sc = sFill * 1.1f;
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x)
                {
                    float px = (x + 0.5f) * W / bw - W * 0.5f, py = (y + 0.5f) * H / bh - H * 0.5f, o[4];
                    if (sampleFrame (*fr, rot, px / sc + rw * 0.5f, py / sc + rh * 0.5f, o))
                        for (int k = 0; k < 3; ++k) bg[((size_t) y * bw + x) * 3 + k] = o[k] * 0.55f;
                }
            boxBlur (bg, bw, bh, std::max (1, bw / 40), std::max (1, bh / 40), 3);
        }

        parallelRows (H, [&] (int y) {
            for (int x = 0; x < W; ++x)
            {
                float col[4] { 0, 0, 0, 0 }, acc[4] { 0, 0, 0, 0 };
                int got = 0;
                for (int k = 0; k < taps; ++k)
                {
                    float px = x + 0.5f - W * 0.5f - offX - (taps > 1 ? tapLen * ((float) k / (taps - 1) - 0.5f) : 0.0f);
                    float py = y + 0.5f - H * 0.5f - offY;
                    float ux = (px * cr - py * sr) / sclX / trStretch, uy = (px * sr + py * cr) / sclY;
                    if (mirror) ux = -ux;
                    if (trMirror > 0) { float f = trDir < 2 ? std::abs (ux) : ux; ux = mixf (ux, f, trMirror); if (trDir >= 2) uy = mixf (uy, std::abs (uy), trMirror); }
                    if (cropped)
                    {
                        float cx = ux + rw * 0.5f, cy = uy + rh * 0.5f;
                        if (cx < cL * rw || cx > (1 - cR) * rw || cy < cT * rh || cy > (1 - cB) * rh) continue;
                    }
                    float o[4];
                    if (sampleFrame (*fr, rot, ux + rw * 0.5f, uy + rh * 0.5f, o)) { for (int q = 0; q < 4; ++q) acc[q] += o[q]; ++got; }
                }
                if (got > 0) for (int q = 0; q < 4; ++q) col[q] = acc[q] / taps;
                if (reframe == 2)
                {
                    float b[3]; sampleBuf (bg, bw, bh, (x + 0.5f) * bw / W - 0.5f, (y + 0.5f) * bh / H - 0.5f, b);
                    for (int q = 0; q < 3; ++q) col[q] = col[q] * col[3] + b[q] * (1 - col[3]);
                    col[3] = 1;
                }
                float a = col[3] * alpha0;
                if (a <= 0.0005f) continue;
                // clip grade
                for (int q = 0; q < 3; ++q) col[q] = (col[q] * cExp - 0.45f) * cCon + 0.45f;
                float l = luma (col);
                for (int q = 0; q < 3; ++q) col[q] = clamp01 (l + (col[q] - l) * cSat);
                if (cTemp != 0 || cTint != 0)
                {
                    col[0] += cTemp * 0.08f + cTint * 0.03f; col[2] += -cTemp * 0.08f + cTint * 0.03f; col[1] -= cTint * 0.06f;
                }
                adjustTones (col, cBri, cHi, cSh);
                if (cCurve.active) applyCurve (cCurve, col);
                for (int q = 0; q < 3; ++q) col[q] = clamp01 (col[q]);
                if (chroma > 0)
                {
                    float h, sat, v; rgbToHsv (col, h, sat, v);
                    float hd = std::abs (std::fmod (h - keyHue + 540.0f, 360.0f) - 180.0f);
                    float k = (1 - smooth (keyTol * 60, keyTol * 60 + 25, hd)) * smooth (0.08f, 0.3f, sat) * smooth (0.05f, 0.2f, v);
                    a *= 1 - chroma * k;
                    // spill suppression on the key channel
                    if (keyHue > 60 && keyHue < 180) col[1] = mixf (col[1], std::min (col[1], std::max (col[0], col[2])), chroma);
                    else if (keyHue >= 180 && keyHue < 300) col[2] = mixf (col[2], std::min (col[2], std::max (col[0], col[1])), chroma);
                }
                if (lumaK > 0) a *= 1 - lumaK * (1 - smooth (lumaT - 0.08f, lumaT + 0.08f, luma (col)));
                if (maskShape > 0)
                {
                    float mx = ((x + 0.5f) / W - 0.5f - cv ("posX") * 0.5f) / (mW * 0.5f);
                    float my = ((y + 0.5f) / H - 0.5f - cv ("posY") * 0.5f) / (mH * 0.5f);
                    float d = maskShape == 1 ? std::sqrt (mx * mx + my * my) : std::max (std::abs (mx), std::abs (my));
                    float m = 1 - smooth (1 - mF, 1.0001f, d);
                    a *= mInv ? 1 - m : m;
                }
                if (trWipe == 1)
                {
                    float lv = luma (col); if (trDir % 2 == 1) lv = 1 - lv;
                    float th = 1.15f - prog * 1.3f;
                    a *= smooth (th - 0.12f, th + 0.12f, lv);
                }
                else if (trWipe == 2)
                {
                    // fractal value noise eats the incoming shot in, with a hot burning edge
                    float n = 0, amp = 0.5f, sum = 0;
                    for (int o = 0; o < 3; ++o)
                    {
                        float cell = minWH / (4.0f * (float) (1 << o));
                        float gx = x / cell, gy = y / cell; int ix = (int) gx, iy = (int) gy; float fx = gx - ix, fy = gy - iy;
                        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
                        float v = mixf (mixf (hash3 (ix, iy, 77 + o), hash3 (ix + 1, iy, 77 + o), fx), mixf (hash3 (ix, iy + 1, 77 + o), hash3 (ix + 1, iy + 1, 77 + o), fx), fy);
                        n += v * amp; sum += amp; amp *= 0.5f;
                    }
                    n /= sum;
                    float th = prog * 1.25f - 0.1f;
                    a *= 1 - smooth (th - 0.025f, th + 0.025f, n);
                    float e = clamp01 (1 - std::abs (n - th - 0.04f) / 0.05f) * std::min (1.0f, trAmt) * (prog < 1 ? 1.0f : 0.0f);
                    col[0] = clamp01 (col[0] + e * 1.0f); col[1] = clamp01 (col[1] + e * 0.45f); col[2] = clamp01 (col[2] + e * 0.08f);
                    if (e > 0) a = std::max (a, e);
                }
                if (trPrism > 0)
                {
                    float k = trPrism, g0 = col[0], g1 = col[1], g2 = col[2];
                    col[0] = mixf (g0, g1, k); col[1] = mixf (g1, g2, k); col[2] = mixf (g2, g0, k);
                }
                if (trLeak > 0)
                {
                    float gx = trDir % 2 == 1 ? 1 - (x + 0.5f) / W : (x + 0.5f) / W;
                    float w = trLeak * smooth (0.15f, 1.0f, gx + 0.25f * std::sin ((y + 0.5f) / H * 3.1f));
                    col[0] = clamp01 (col[0] + w * 1.1f); col[1] = clamp01 (col[1] + w * 0.55f); col[2] = clamp01 (col[2] + w * 0.22f);
                }
                if (transKind == (int) TransitionType::DipBlack) for (int q = 0; q < 3; ++q) col[q] *= prog;
                if (transKind == (int) TransitionType::DipWhite) for (int q = 0; q < 3; ++q) col[q] = mixf (1, col[q], prog);
                if (transKind == (int) TransitionType::Flash) for (int q = 0; q < 3; ++q) col[q] = mixf (col[q], 1, std::min (1.0f, (1 - prog) * (1 - prog) * trAmt));
                float* d = &img[((size_t) y * W + x) * 3];
                float r[3]; blendPixel (blend, d, col, r);
                a = clamp01 (a);
                for (int q = 0; q < 3; ++q) d[q] = mixf (d[q], r[q], a);
            }
        });
    };

    for (auto* c : active)
    {
        float prog = 1;
        if (c->transIn != TransitionType::Cut && c->transLen > 0 && t < c->start + c->transLen)
            prog = (float) juce::jlimit (0.0, 1.0, (double) (t - c->start) / (double) c->transLen);
        const auto tt = c->transIn;
        const bool inTrans = prog < 1;
        trAmt = c->fx.get ("trAmt", 1.0f); trDir = juce::jlimit (0, 3, (int) std::lround (c->fx.get ("trDir", 0.0f)));
        const float sgn = (trDir == 1 || trDir == 3) ? -1.0f : 1.0f;
        const float inv = 1 - prog, inv2 = inv * inv;
        const bool overPrev = inTrans && (tt == TransitionType::Dissolve || tt == TransitionType::LumaWipe || tt == TransitionType::InkBurn
                                          || tt == TransitionType::Blur || tt == TransitionType::Prism);
        if (overPrev)
        {
            const Clip* prev = nullptr;
            Ticks tol = secToTicks (1.5 / p.fps);
            for (auto& o : s.clips) if (o.track == c->track && &o != c && std::abs (o.end() - c->start) <= tol) prev = &o;
            if (prev != nullptr) drawLayer (*prev, 1.0f, 1.0f, 0, 0, 1);
        }
        float alpha = 1.0f, zoom = 1.0f, whip = 0.0f;
        if (inTrans)
        {
            switch (tt)
            {
                case TransitionType::Dissolve: alpha = prog; break;
                case TransitionType::Blur:     alpha = smooth (0.0f, 0.6f, prog); transBlur = std::max (transBlur, inv2 * trAmt); break;
                case TransitionType::Prism:    alpha = smooth (0.0f, 0.5f, prog); trPrism = inv * std::min (1.0f, trAmt); transSplit = std::max (transSplit, inv2 * trAmt); zoom = 1 + 0.12f * inv2 * trAmt; break;
                case TransitionType::LumaWipe: trWipe = 1; break;
                case TransitionType::InkBurn:  trWipe = 2; break;
                case TransitionType::Glitch:   transGlitch = std::max (transGlitch, inv * trAmt); break;
                case TransitionType::RgbSplit: transSplit = std::max (transSplit, inv2 * trAmt * 1.5f); zoom = 1 + 0.05f * inv2 * trAmt; break;
                case TransitionType::Zoom:     zoom = 1 + 0.3f * inv2 * trAmt * (trDir == 1 ? -0.6f : 1.0f); break;
                case TransitionType::Whip:     whip = -0.6f * inv2 * trAmt * sgn; break;
                case TransitionType::Spin:     trRot = sgn * juce::MathConstants<float>::pi * inv2 * trAmt; zoom = 1 + 0.4f * inv2 * trAmt; break;
                case TransitionType::Shake:
                {
                    int fi = (int) std::floor (ticksToSec (t) * 30.0);
                    trShX = (hash3 (fi, 7, 1) - 0.5f) * 0.12f * inv * trAmt; trShY = (hash3 (fi, 9, 2) - 0.5f) * 0.12f * inv * trAmt;
                    zoom = 1 + 0.08f * inv * trAmt; break;
                }
                case TransitionType::Stretch:  trStretch = 1 + 1.5f * inv2 * trAmt; break;
                case TransitionType::Mirror:   trMirror = std::min (1.0f, inv * 1.4f * trAmt); break;
                case TransitionType::LightLeak: trLeak = std::sin (juce::MathConstants<float>::pi * std::sqrt (prog)) * 0.9f * trAmt; break;
                default: break;
            }
        }
        int tk = inTrans ? (int) tt : 0;
        drawLayer (*c, alpha, zoom, whip, tk, prog);
        trRot = 0; trStretch = 1; trShX = trShY = 0; trMirror = 0; trPrism = 0; trLeak = 0; trWipe = 0;
    }
    if (transBlur > 0.001f)
    {
        int r = std::max (1, (int) std::lround (transBlur * minWH * 0.025f));
        boxBlur (img, W, H, r, r, 3);
    }

    // ---- warps: kaleidoscope + liquid
    float kal = sv ("kaleido"), liq = sv ("liquid");
    if (kal > 0 || liq > 0)
    {
        auto src = img;
        float spin = (float) T * 0.25f;
        parallelRows (H, [&] (int y) {
            for (int x = 0; x < W; ++x)
            {
                float sx = (float) x, sy = (float) y;
                if (liq > 0)
                {
                    float k = liq * (1 + be.kick);
                    sx += std::sin ((float) y / H * 11 + (float) T * 2.1f) * k * 0.025f * W;
                    sy += std::cos ((float) x / W * 9 + (float) T * 1.7f) * k * 0.02f * H;
                }
                float out[3]; sampleBuf (src, W, H, sx, sy, out);
                if (kal > 0)
                {
                    float cx = (x - W * 0.5f) / minWH, cy = (y - H * 0.5f) / minWH;
                    float rr = std::sqrt (cx * cx + cy * cy), an = std::atan2 (cy, cx) + spin;
                    const float seg = juce::MathConstants<float>::twoPi / 6;
                    an = std::fmod (an + 100 * seg, seg); if (an > seg * 0.5f) an = seg - an;
                    float kx = W * 0.5f + std::cos (an) * rr * minWH, ky = H * 0.5f + std::sin (an) * rr * minWH;
                    float o2[3]; sampleBuf (src, W, H, kx, ky, o2);
                    float m = std::min (1.0f, kal * 1.5f);
                    for (int q = 0; q < 3; ++q) out[q] = mixf (out[q], o2[q], m);
                }
                std::copy (out, out + 3, &img[((size_t) y * W + x) * 3]);
            }
        });
    }

    // ---- glitch blocks, RGB split, VHS (horizontal resampling pass)
    float glitch = std::max (transGlitch, sv ("glitch") * (0.35f + be.snare) + sv ("beatGlitch") * be.snare);
    float split = (sv ("rgbSplit") * 0.006f + sv ("beatGlitch") * be.snare * 0.012f + glitch * 0.01f + transSplit * 0.02f) * W;
    float vhs = sv ("vhs");
    if (glitch > 0.01f || split > 0.25f || vhs > 0)
    {
        auto src = img;
        int bandH = std::max (4, H / 28);
        int gseed = frameIdx / 2;
        float trackY = (float) std::fmod (T * 0.13, 1.0) * H * 1.3f - H * 0.15f;
        parallelRows (H, [&] (int y) {
            float off = 0;
            int band = y / bandH;
            bool gBand = glitch > 0.01f && hash3 (band, gseed, 11) < glitch * 0.45f;
            if (gBand) off += (hash3 (band, gseed, 12) - 0.5f) * W * 0.18f * glitch;
            if (vhs > 0)
            {
                off += (std::sin (y * 0.07f + (float) T * 5) * 0.5f + (hash3 (y / 3, frameIdx, 13) - 0.5f) * 0.6f) * vhs * W * 0.003f;
                float dy = std::abs (y - trackY);
                if (dy < H * 0.03f) off += (hash3 (y, frameIdx, 14) - 0.3f) * vhs * W * 0.02f;
            }
            const float* row = &src[(size_t) y * W * 3];
            auto at = [&] (float xx, int c) {
                xx = juce::jlimit (0.0f, (float) W - 1.001f, xx); int x0 = (int) xx; float f = xx - x0;
                return mixf (row[x0 * 3 + c], row[std::min (W - 1, x0 + 1) * 3 + c], f);
            };
            float chromaStep = vhs * W * 0.0025f;
            for (int x = 0; x < W; ++x)
            {
                float bx = x + off;
                float c[3] { at (bx + split + vhs * W * 0.002f, 0), at (bx, 1), at (bx - split, 2) };
                if (gBand && hash3 (band, gseed, 15) < 0.3f) std::swap (c[0], c[2]);
                if (vhs > 0)
                {
                    float avg[3] { 0, 0, 0 };
                    for (int k = -2; k <= 2; ++k) for (int q = 0; q < 3; ++q) avg[q] += at (bx + k * chromaStep + chromaStep * 1.5f, q) * 0.2f;
                    float dl = luma (c) - luma (avg);
                    for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], avg[q] + dl, vhs);
                    float n = (hash3 (x / 2, y, frameIdx) - 0.5f) * vhs * 0.12f;
                    for (int q = 0; q < 3; ++q) c[q] += n;
                }
                std::copy (c, c + 3, &img[((size_t) y * W + x) * 3]);
            }
        });
    }

    // ---- grade + LUT + acid color
    auto lut = lutFor (postFx.lutPath);
    const float gExp = std::pow (2.0f, sv ("exposure")) * (1 - sv ("hatStrobe") * 0.35f * be.hat);
    const float temp = sv ("temperature"), tint = sv ("tint"), con = sv ("contrast"), sat = sv ("saturation");
    const float fade = sv ("fade"), splitT = sv ("splitTone"), crush = sv ("crush"), lutMix = sv ("lutMix");
    const float hueC = sv ("hueCycle"), post = sv ("posterize"), sol = sv ("solarize"), therm = sv ("thermal");
    bool invert = sv ("invertHits") > 0 && be.snareIdx >= 0 && be.sinceSnare < 2.0 / p.fps && hash3 (be.snareIdx, 77, 1) < sv ("invertHits");
    float hueAng = hueC * ((float) T * 1.4f + be.kickIdx * 0.6f);
    const float gBri = sv ("brightness"), gHi = sv ("highlights"), gSh = sv ("shadows");
    const ToneCurve gCurve = makeCurve (postFx, SP);
    float ch = std::cos (hueAng), shh = std::sin (hueAng);
    parallelRows (H, [&] (int y) {
        for (int x = 0; x < W; ++x)
        {
            float* c = &img[((size_t) y * W + x) * 3];
            for (int q = 0; q < 3; ++q) c[q] *= gExp;
            c[0] += temp * 0.08f; c[2] -= temp * 0.08f;
            c[1] -= tint * 0.06f; c[0] += tint * 0.03f; c[2] += tint * 0.03f;
            for (int q = 0; q < 3; ++q) c[q] = (c[q] - 0.45f) * con + 0.45f;
            float l = luma (c);
            for (int q = 0; q < 3; ++q) c[q] = l + (c[q] - l) * sat;
            adjustTones (c, gBri, gHi, gSh);
            if (splitT > 0)
            {
                float lw = clamp01 (l);
                c[0] += splitT * 0.12f * ((1 - lw) * -0.45f + lw * 0.3f);
                c[1] += splitT * 0.12f * ((1 - lw) * 0.08f + lw * 0.06f);
                c[2] += splitT * 0.12f * ((1 - lw) * 0.3f + lw * -0.3f);
            }
            if (crush > 0)
                for (int q = 0; q < 3; ++q) { float v = std::max (0.0f, (c[q] - crush * 0.1f) / (1 - crush * 0.1f)); c[q] = std::pow (v, 1 + crush * 0.5f); }
            if (fade > 0) for (int q = 0; q < 3; ++q) c[q] = fade * 0.14f + c[q] * (1 - fade * 0.2f);
            for (int q = 0; q < 3; ++q) c[q] = clamp01 (c[q]);
            if (gCurve.active) applyCurve (gCurve, c);
            if (lut != nullptr && lutMix > 0)
            {
                int n = lut->size; float fx = c[0] * (n - 1), fy = c[1] * (n - 1), fz = c[2] * (n - 1);
                int x0 = std::min (n - 2, (int) fx), y0 = std::min (n - 2, (int) fy), z0 = std::min (n - 2, (int) fz);
                float ax = fx - x0, ay = fy - y0, az = fz - z0;
                auto L = [&] (int i, int j, int k, int ch2) { return lut->rgb[(((size_t) k * n + j) * n + i) * 3 + ch2]; };
                for (int q = 0; q < 3; ++q)
                {
                    float v = mixf (mixf (mixf (L (x0, y0, z0, q), L (x0 + 1, y0, z0, q), ax), mixf (L (x0, y0 + 1, z0, q), L (x0 + 1, y0 + 1, z0, q), ax), ay),
                                    mixf (mixf (L (x0, y0, z0 + 1, q), L (x0 + 1, y0, z0 + 1, q), ax), mixf (L (x0, y0 + 1, z0 + 1, q), L (x0 + 1, y0 + 1, z0 + 1, q), ax), ay), az);
                    c[q] = mixf (c[q], v, lutMix);
                }
            }
            if (hueC > 0)
            {
                // YIQ hue rotation
                float Y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                float I = 0.596f * c[0] - 0.274f * c[1] - 0.322f * c[2];
                float Q = 0.211f * c[0] - 0.523f * c[1] + 0.312f * c[2];
                float I2 = I * ch - Q * shh, Q2 = I * shh + Q * ch;
                c[0] = Y + 0.956f * I2 + 0.621f * Q2; c[1] = Y - 0.272f * I2 - 0.647f * Q2; c[2] = Y - 1.106f * I2 + 1.703f * Q2;
            }
            if (post > 0)
            {
                float lv = 2 + (1 - post) * 10;
                for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], std::floor (clamp01 (c[q]) * lv + 0.5f) / lv, std::min (1.0f, post * 2));
            }
            if (sol > 0) for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], c[q] > 0.5f ? 1 - c[q] : c[q], sol);
            if (therm > 0) { float tc[3]; thermalColor (luma (c), tc); for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], tc[q], therm); }
            if (invert) for (int q = 0; q < 3; ++q) c[q] = 1 - clamp01 (c[q]);
        }
    });

    // ---- edge glow (Sobel on luma), neon hue sweeping across the frame
    float eg = sv ("edgeGlow");
    if (eg > 0)
    {
        std::vector<float> L (NP);
        for (size_t i = 0; i < NP; ++i) L[i] = luma (&img[i * 3]);
        parallelRows (H, [&] (int y) {
            for (int x = 0; x < W; ++x)
            {
                auto P = [&] (int xx, int yy) { return L[(size_t) juce::jlimit (0, H - 1, yy) * W + juce::jlimit (0, W - 1, xx)]; };
                float gx = P (x + 1, y - 1) + 2 * P (x + 1, y) + P (x + 1, y + 1) - P (x - 1, y - 1) - 2 * P (x - 1, y) - P (x - 1, y + 1);
                float gy = P (x - 1, y + 1) + 2 * P (x, y + 1) + P (x + 1, y + 1) - P (x - 1, y - 1) - 2 * P (x, y - 1) - P (x + 1, y - 1);
                float e = clamp01 (std::sqrt (gx * gx + gy * gy) * 1.5f);
                float h = std::fmod ((float) x / W * 0.6f + (float) y / H * 0.3f + (float) T * 0.15f, 1.0f) * 6.283f;
                float nc[3] { 0.5f + 0.5f * std::cos (h), 0.5f + 0.5f * std::cos (h + 2.09f), 0.5f + 0.5f * std::cos (h + 4.19f) };
                float* c = &img[((size_t) y * W + x) * 3];
                for (int q = 0; q < 3; ++q) c[q] = c[q] * (1 - eg * 0.65f) + e * nc[q] * eg * 1.8f;
            }
        });
    }

    // ---- bloom / halation / anamorphic flare from a quarter-res highlight buffer
    float bloom = sv ("bloom"), hal = sv ("halation"), flare = sv ("flare");
    if (bloom > 0 || hal > 0 || flare > 0)
    {
        int hw = std::max (4, W / 4), hh = std::max (4, H / 4);
        std::vector<float> hi ((size_t) hw * hh * 3, 0.0f), fl;
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < hw; ++x)
            {
                float c[3]; sampleBuf (img, W, H, x * 4.0f + 1.5f, y * 4.0f + 1.5f, c);
                float k = std::pow (clamp01 ((luma (c) - 0.6f) / 0.4f), 2.0f);
                for (int q = 0; q < 3; ++q) hi[((size_t) y * hw + x) * 3 + q] = c[q] * k;
            }
        if (flare > 0)
        {
            fl = hi;
            for (auto& v : fl) v = v * v * 2.0f;
            boxBlur (fl, hw, hh, std::max (2, hw / 10), 0, 3);
        }
        boxBlur (hi, hw, hh, std::max (1, hw / 60), std::max (1, hw / 60), 3);
        parallelRows (H, [&] (int y) {
            for (int x = 0; x < W; ++x)
            {
                float* c = &img[((size_t) y * W + x) * 3];
                float b[3]; sampleBuf (hi, hw, hh, (x - 1.5f) / 4.0f, (y - 1.5f) / 4.0f, b);
                float bl = luma (b);
                for (int q = 0; q < 3; ++q) c[q] += b[q] * bloom * 1.1f;
                c[0] += bl * hal * 1.1f; c[1] += bl * hal * 0.3f; c[2] += bl * hal * 0.12f;
                if (flare > 0)
                {
                    float f[3]; sampleBuf (fl, hw, hh, (x - 1.5f) / 4.0f, (y - 1.5f) / 4.0f, f);
                    float fv = luma (f) * flare * 2.5f;
                    c[0] += fv * 0.35f; c[1] += fv * 0.55f; c[2] += fv * 1.0f;
                }
            }
        });
    }

    // ---- echo trails (frame feedback; resets when the playhead jumps)
    float trails = sv ("trails");
    if (trails > 0)
    {
        bool cont = trailT >= 0 && trailW == W && trailH == H && t > trailT && ticksToSec (t - trailT) < 0.25;
        if (cont)
        {
            float decay = 0.72f + 0.22f * trails;
            for (size_t i = 0; i < NP * 3; ++i) img[i] = mixf (img[i], std::max (img[i], trail[i] * decay), trails);
        }
        trail = img; trailT = t; trailW = W; trailH = H;
    }

    // ---- generated overlays: light leaks, film burn on cuts, dust & scratches, snare flash
    float leak = sv ("lightLeak"), burn = sv ("filmBurn"), dust = sv ("dust"), flash = sv ("snareFlash") * 0.75f * be.snareFast;
    float burnI = 0;
    if (burn > 0)
    {
        Ticks lastCut = -1;
        for (auto& c : s.clips) if (c.start > 0 && c.start <= t && c.track >= 0 && c.track < (int) s.tracks.size() && s.tracks[(size_t) c.track].kind == TrackKind::Video) lastCut = std::max (lastCut, c.start);
        if (lastCut >= 0) { double dt = ticksToSec (t - lastCut); if (dt < 0.6) burnI = burn * (float) std::exp (-dt / 0.16); }
    }
    if (leak > 0 || burnI > 0.01f || flash > 0.002f)
    {
        float l1x = 0.5f + 0.55f * std::sin ((float) T * 0.17f + 1.3f), l1y = 0.2f + 0.3f * std::sin ((float) T * 0.11f);
        float l2x = 0.5f + 0.6f * std::sin ((float) T * 0.09f + 4.0f), l2y = 0.85f + 0.2f * std::cos ((float) T * 0.13f);
        float pulse = 0.75f + 0.25f * std::sin ((float) T * 0.7f) + 0.2f * be.kick;
        int burnSide = (int) (hash3 ((int) (t / secToTicks (0.5)), 3, 9) * 4);
        parallelRows (H, [&] (int y) {
            for (int x = 0; x < W; ++x)
            {
                float* c = &img[((size_t) y * W + x) * 3];
                float u = (float) x / W, v = (float) y / H, add[3] { 0, 0, 0 };
                if (leak > 0)
                {
                    float d1 = ((u - l1x) * (u - l1x) * 1.2f + (v - l1y) * (v - l1y) * 2.0f), d2 = ((u - l2x) * (u - l2x) + (v - l2y) * (v - l2y) * 1.6f);
                    float i1 = std::exp (-d1 / 0.06f) * pulse * leak, i2 = std::exp (-d2 / 0.09f) * pulse * leak * 0.8f;
                    add[0] += i1 * 1.0f + i2 * 1.0f; add[1] += i1 * 0.42f + i2 * 0.7f; add[2] += i1 * 0.12f + i2 * 0.3f;
                }
                if (burnI > 0.01f)
                {
                    float e = burnSide == 0 ? u : burnSide == 1 ? 1 - u : burnSide == 2 ? v : 1 - v;
                    float n = hash3 (x / 24, y / 24, frameIdx / 3) * 0.25f;
                    float k = smooth (0.45f, 1.0f, e + n) * burnI;
                    add[0] += k * 1.3f; add[1] += k * 0.7f + k * k * 0.5f; add[2] += k * 0.2f + k * k * 0.4f;
                }
                for (int q = 0; q < 3; ++q) { float a = clamp01 (add[q]); c[q] = 1 - (1 - clamp01 (c[q])) * (1 - a); }
                if (flash > 0.002f) for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], 1.0f, flash);
            }
        });
    }
    if (dust > 0)
    {
        float px = std::max (1.0f, H / 720.0f);
        int specks = (int) (dust * 45);
        for (int i = 0; i < specks; ++i)
        {
            if (hash3 (i, frameIdx, 21) > 0.6f) continue;
            int cx = (int) (hash3 (i, frameIdx, 22) * W), cy = (int) (hash3 (i, frameIdx, 23) * H);
            float r = px * (0.6f + hash3 (i, frameIdx, 24) * 2.2f);
            bool bright = hash3 (i, frameIdx, 25) < 0.35f;
            int R = (int) std::ceil (r) + 1;
            for (int y = std::max (0, cy - R); y < std::min (H, cy + R); ++y)
                for (int x = std::max (0, cx - R); x < std::min (W, cx + R); ++x)
                {
                    float d = std::sqrt ((float) ((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                    float a = (1 - smooth (r * 0.5f, r, d)) * 0.8f * dust;
                    float* c = &img[((size_t) y * W + x) * 3];
                    for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], bright ? 0.95f : 0.04f, a);
                }
        }
        int scratches = hash3 (frameIdx, 5, 26) < dust ? 1 + (int) (hash3 (frameIdx, 5, 27) * 2) : 0;
        for (int i = 0; i < scratches; ++i)
        {
            float sx = hash3 (frameIdx / 3, i, 28) * W;
            for (int y = 0; y < H; ++y)
            {
                int x = (int) (sx + std::sin (y * 0.01f + i) * px * 3);
                if (x < 0 || x >= W) continue;
                float* c = &img[((size_t) y * W + x) * 3];
                for (int q = 0; q < 3; ++q) c[q] = mixf (c[q], 0.9f, 0.35f * dust);
            }
        }
    }

    // ---- sharpen (unsharp mask against a 4-neighbour blur)
    if (float shp = sv ("sharpen"); shp > 0)
    {
        auto src = img;
        parallelRows (H, [&] (int y) {
            const size_t ya = (size_t) std::max (0, y - 1) * W, yb = (size_t) std::min (H - 1, y + 1) * W, yc = (size_t) y * W;
            for (int x = 0; x < W; ++x)
            {
                const size_t xa = (size_t) std::max (0, x - 1), xb = (size_t) std::min (W - 1, x + 1);
                for (int q = 0; q < 3; ++q)
                {
                    float b = (src[(ya + x) * 3 + q] + src[(yb + x) * 3 + q] + src[(yc + xa) * 3 + q] + src[(yc + xb) * 3 + q]) * 0.25f;
                    float& d = img[(yc + x) * 3 + q];
                    d += (d - b) * shp * 1.6f;
                }
            }
        });
    }

    // ---- vignette, grain, letterbox, output
    float vig = sv ("vignette"), grain = sv ("grain"), gSize = sv ("grainSize"), lbox = sv ("letterbox");
    int cell = std::max (1, (int) std::lround ((1 + gSize * 1.2f) * H / 1080.0f));
    int bar = 0;
    if (lbox > 0)
    {
        float target = sv ("letterboxRatio"), cur = (float) W / H;
        if (target < 1.0f) target = 2.39f;
        if (cur < target) bar = (int) std::lround (H * (1 - cur / target) * 0.5f * std::min (1.0f, lbox * 2));
    }
    rgba.resize (NP * 4);
    parallelRows (H, [&] (int y) {
        for (int x = 0; x < W; ++x)
        {
            float* c = &img[((size_t) y * W + x) * 3];
            if (vig > 0)
            {
                float u = (x + 0.5f) / W - 0.5f, v = (y + 0.5f) / H - 0.5f;
                float k = 1 - vig * 0.85f * smooth (0.25f, 0.85f, std::sqrt (u * u + v * v) * 1.4f);
                for (int q = 0; q < 3; ++q) c[q] *= k;
            }
            if (grain > 0)
            {
                float n = hash3 (x / cell, y / cell, frameIdx) + hash3 (x / cell, y / cell, frameIdx + 9173) - 1.0f;
                float l = clamp01 (luma (c));
                float g = n * grain * 0.14f * (0.35f + 0.65f * (1 - std::abs (2 * l - 1)));
                for (int q = 0; q < 3; ++q) c[q] += g;
            }
            if (y < bar || y >= H - bar) c[0] = c[1] = c[2] = 0;
            auto* o = &rgba[((size_t) y * W + x) * 4];
            for (int q = 0; q < 3; ++q) o[q] = (juce::uint8) (clamp01 (c[q]) * 255.0f + 0.5f);
            o[3] = 255;
        }
    });
    return true;
}
}
