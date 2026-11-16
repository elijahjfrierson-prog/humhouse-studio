#pragma once
#include "Model.h"
#include "AudioMix.h"
#include <atomic>
#include <functional>

struct AVFormatContext; struct AVCodecContext; struct AVStream; struct AVFrame; struct AVPacket; struct SwsContext;

namespace isfx
{
// H.264 (hardware first: VideoToolbox / MediaFoundation, then libopenh264/libx264, then MPEG-4 fallback) + AAC in MP4.
class VideoEncoder
{
public:
    ~VideoEncoder();
    // codec: h264 (MP4/AAC), h265, h265_lossless, h264_lossless, prores_hq, prores_4444 (MOV + uncompressed 24-bit PCM)
    bool open (const juce::File& f, int w, int h, double fps, int mbps, bool withAudio, juce::String& error, const juce::String& codec = "h264");
    bool writeVideo (const juce::uint8* rgbaTopDown, juce::int64 frameIndex);
    bool writeAudio (const float* L, const float* R, int n);
    bool finish (juce::String& error);
    juce::String codecName() const { return vcodecName; }
private:
    bool encode (AVCodecContext*, AVStream*, AVFrame*);
    bool flushAudio (bool final);
    AVFormatContext* fmt = nullptr;
    AVCodecContext *venc = nullptr, *aenc = nullptr;
    AVStream *vst = nullptr, *ast = nullptr;
    AVFrame *vfr = nullptr, *afr = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int W = 0, H = 0;
    std::vector<float> pendL, pendR;
    juce::int64 audioPts = 0;
    juce::String vcodecName;
    bool headerWritten = false;
};

// export formats this build can encode (lossless H.264/H.265 need libx264/libx265, absent from LGPL builds)
bool exportCodecAvailable (const juce::String& codec);

struct ExportJob
{
    juce::File output;
    int width = 1920, height = 1080;
    double fps = 30.0;
    int mbps = 20;
    juce::String codec = "h264";
    Ticks rangeStart = 0, rangeEnd = 0;   // end 0 = sequence end
    std::function<void (float* L, float* R, int n)> audioFx;   // master plug-in chain (optional)
    TrackFxHost* trackFx = nullptr;                            // per-track plug-in chains (optional)
};

// renderFrame(time, rgbaOut) must fill width*height*4 bytes (top-down RGBA)
using FrameRenderFn = std::function<bool (Ticks, std::vector<juce::uint8>&)>;
using ProgressFn = std::function<void (double)>;

bool runExport (const Sequence& s, const AudioCache& audio, const ExportJob& job, FrameRenderFn render,
                ProgressFn progress, std::atomic<bool>& cancel, juce::String& error, juce::String* codecUsed = nullptr);
}
