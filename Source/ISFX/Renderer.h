#pragma once
#include "Model.h"
#include "Media.h"
#include "Looks.h"
#include <mutex>

namespace isfx
{
// in-between frame at f (0 = a, 1 = b): cross-blend, or block-matched optical flow warp of both frames
void interpolateFrames (const Frame& a, const Frame& b, float f, bool flow, Frame& out);
}

namespace isfx
{
struct AspectPreset { const char* name; int w, h; const char* use; };
const std::vector<AspectPreset>& aspectPresets();
// output size for the project aspect with the given short side (1080 = HD, 2160 = 4K), even dimensions
void outputSize (const Project& p, int shortSide, int& W, int& H);

juce::StringArray blendModeNames();
// split-screen slots (e.g. 9:16 with one clip on top and another underneath): sets scale/position and a slot mask
juce::StringArray layoutNames();

// where a clip lands in a W x H output frame (same maths as the renderer): centre, rotation and the cropped box in
// clip-local pixels. Used by the preview's on-video transform handles.
struct ClipGeom
{
    float cx = 0, cy = 0, cr = 1, sr = 0, hx = 0, hy = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    juce::Point<float> toScreen (float lx, float ly) const { return { cx + lx * cr + ly * sr, cy - lx * sr + ly * cr }; }
    juce::Point<float> toLocal (juce::Point<float> p) const { float px = p.x - cx, py = p.y - cy; return { px * cr - py * sr, px * sr + py * cr }; }
    bool contains (juce::Point<float> p) const { auto l = toLocal (p); return l.x >= x0 && l.x <= x1 && l.y >= y0 && l.y <= y1; }
};
bool clipGeometry (const Project&, const Clip&, float W, float H, ClipGeom&);
void applyLayout (const Project& p, Clip& c, int slot);
void applyLayoutRect (const Project& p, Clip& c, float x, float y, float w, float h);   // fit the clip into a frame rectangle (0..1) and mask to it
// collages over the given clips (top track first): 0 = 3x3 grid, 1 = auto grid, 2 = picture-in-picture, 3 = film strip
void applyCollage (const Project& p, std::vector<Clip*> clips, int kind);

// Composites a sequence frame: tracks + transforms + keys/masks + blend modes, then grade, LUT, film,
// beat-reactive, acid and generated overlay FX. Preview and export use this same code path.
constexpr int kProxyHeight = 540;

// Clips whose live frame is expensive (reverse, optical flow, warps, effect layers): shown red in the ruler until rendered
bool needsRender (const Project&, const Sequence&, const Clip&);

class Renderer
{
public:
    // decodeH caps decoded source height (preview uses a small value, export the output height)
    bool render (const Project& p, const Sequence& s, Ticks t, int W, int H, std::vector<juce::uint8>& rgba, int decodeH);
    void clearSources();   // close decoders (project changed / media relinked)
    // Live playback: decode on background threads ahead of the playhead and pre-roll upcoming cuts; render never
    // waits on a decoder (a late frame repeats the last one). Paused/export rendering stays synchronous and exact.
    void setLive (bool b) { live = b; }
    int lateFrames() const { return late; }
private:
    struct Source
    {
        std::unique_ptr<VideoDecoder> dec;
        std::unique_ptr<PrefetchDecoder> pre;
        std::shared_ptr<const Frame> shown, shownNext;
        double fps = 30;
        Frame still, last;
        bool failed = false;
        int rotation = 0;
        juce::uint32 used = 0;
        int opaque = -1;    // stills: -1 unknown, 0 has transparency, 1 opaque
    };
    static constexpr size_t kMaxOpenSources = 8;   // each 4K decoder holds ~100+ MB of frame pools
    juce::uint32 renderStart = 0;
    bool live = false;
    int late = 0;
    juce::uint32 useClock = 0;
    const Frame* frameFor (const Project& p, const juce::String& mediaId, Ticks srcT, int decodeH, int& rotation, bool* opaque = nullptr,
                          bool primeOnly = false, const Frame** after = nullptr, bool reverse = false);
    std::shared_ptr<Lut3D> lutFor (const juce::String& path);

    std::map<juce::String, std::unique_ptr<Source>> sources;
    std::map<juce::String, std::shared_ptr<Lut3D>> luts;
    std::vector<float> trail;
    Ticks trailT = -1;
    int trailW = 0, trailH = 0;
};
}
