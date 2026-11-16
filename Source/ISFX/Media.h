#pragma once
#include <deque>
#include <memory>
#include <functional>
#include "Model.h"
#include <juce_graphics/juce_graphics.h>

struct AVFormatContext; struct AVCodecContext; struct AVFrame; struct AVPacket; struct SwsContext;
struct AVBufferRef;

namespace isfx
{
struct ProbeResult
{
    bool ok = false;
    juce::String error;
    MediaKind kind = MediaKind::Video;
    Ticks duration = 0;
    double fps = 30.0;
    int width = 0, height = 0, rotation = 0;
    bool hasAudio = false, hasVideo = false;
};

ProbeResult probeMedia (const juce::File& f);
bool isMediaFile (const juce::File& f);

// Decodes the whole audio stream, resampled to `rate`, mixed to `channels`, interleaved float.
bool decodeAudio (const juce::File& f, int rate, int channels, std::vector<float>& out, juce::String& error);

// RGBA8 frame (top-down), with the source presentation time.
struct Frame
{
    int w = 0, h = 0;
    std::vector<juce::uint8> rgba;
    Ticks pts = -1;
    bool valid() const { return w > 0 && h > 0; }
};

// Sequential-friendly decoder: seeks only when the requested time jumps backwards or far ahead.
class VideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder();
    bool open (const juce::File& f, int maxHeight, juce::String& error);  // maxHeight 0 = native
    void close();
    bool isOpen() const { return fmt != nullptr; }
    // returns the frame showing at source time t (the last frame with pts <= t)
    bool frameAt (Ticks t, Frame& out);
    bool seekTo (Ticks t);
    const Frame* frameRef (Ticks t);  // valid until the next call
    const Frame* peekNext();          // the frame after the last frameRef (no seek), for slow-motion interpolation
    bool readNext (Frame& out) { return isOpen() && ! eof && decodeNext (out); }
    int width() const { return outW; }
    int height() const { return outH; }
    int rotation() const { return rot; }
    double fps() const { return frameRate; }
private:
    bool decodeNext (Frame& out);
    bool receive (AVFrame* into, Ticks& pts);
    void convert (const AVFrame* src, Frame& out);
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int stream = -1, outW = 0, outH = 0, rot = 0, maxH = 0;
    double frameRate = 30.0, tb = 0;
    Frame cur, next;
    AVFrame* swAv = nullptr;
    AVBufferRef* hwDev = nullptr;
    int hwFmt = -1;
public:
    bool usingHardware() const { return hwDev != nullptr; }
private:
    AVFrame* curAv = nullptr;   // decoded but not yet converted: frames we skip past never pay for RGBA conversion
    AVFrame* nextAv = nullptr;
    Ticks curPts = -1, nextPts = -1;
    bool eof = false, haveNext = false, curConverted = false, fastScale = false;
};

// Playback decoder: a background thread keeps the next few frames converted ahead of the requested time, and
// pre-rolls clips before their cut, so the render thread never waits on a seek or a decode (late = show last frame).
class PrefetchDecoder : private juce::Thread
{
public:
    PrefetchDecoder();
    ~PrefetchDecoder() override;
    bool open (const juce::File& f, int maxHeight, juce::String& error);
    int rotation() const { return rot; }
    void prime (Ticks t);                                   // start decoding at t, don't wait
    // newest decoded frame at or before t (or just after, after a pre-roll); waits up to waitMs if none is ready
    std::shared_ptr<const Frame> get (Ticks t, int waitMs);
    std::shared_ptr<const Frame> after (Ticks pts);          // first queued frame later than pts, never waits
private:
    void run() override;
    VideoDecoder dec;
    int rot = 0;
    Ticks frameDur = 1;
    juce::CriticalSection lock;
    juce::WaitableEvent wake, ready;
    std::deque<std::shared_ptr<Frame>> q;
    std::vector<std::shared_ptr<Frame>> pool;
    Ticks target = -1;
    bool atEnd = false;
    size_t maxAhead = 6;     // shrinks for big frames so RAM stays bounded (~48 MB per source)
    size_t frameBytes = 0;
    void runReverse (Ticks t);
public:
    // Reversed clips: decode in short forward chunks just below the playhead and serve them backwards, so a long-GOP
    // file costs one seek per chunk instead of one seek + GOP re-decode per frame.
    bool reverse = false;
};

juce::Image frameToImage (const Frame& f, int rotation);
// Intra-only MJPEG edit proxy (every frame a keyframe: instant seeks, light decode) for smooth preview playback.
juce::File proxyFileFor (const juce::File& source, int height);
bool makeProxy (const juce::File& source, const juce::File& dest, int height, double durationSec,
                const std::function<bool (double)>& progress, juce::String& error);
juce::Image makeThumbnail (const juce::File& f, Ticks t, int height);

// Peak overview for waveform drawing (min/max pairs per bucket).
struct Waveform
{
    std::vector<float> peaks;  // abs peak per bucket
    double bucketsPerSec = 200.0;
};
Waveform buildWaveform (const std::vector<float>& mono, int rate, double bucketsPerSec = 200.0);
// Warp Stabilizer analysis: per analysed frame, the correction to apply (dx/dy = image shift as a fraction of the frame,
// positive = right/down; rot = degrees, positive = clockwise) at clip-relative timeline ticks. Background thread only.
struct ShakeFix { std::vector<Ticks> t; std::vector<float> dx, dy, rot; float zoom = 1.0f; };
bool analyzeShake (const juce::File& file, Ticks srcIn, Ticks srcLen, double speed, float smoothSec, ShakeFix& out,
                   std::function<bool()> cancel = {}, std::function<void (double)> progress = {});
}
