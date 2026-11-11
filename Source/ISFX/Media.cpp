#include "Media.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <algorithm>

#if LIBAVFORMAT_VERSION_MAJOR >= 59
 using ISFXCodecPtr = const AVCodec*;
#else
 using ISFXCodecPtr = AVCodec*;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
 #define ISFX_NEW_CHLAYOUT 1
#else
 #define ISFX_NEW_CHLAYOUT 0
#endif

namespace isfx
{
static juce::String avErr (int e) { char b[256] = {}; av_strerror (e, b, sizeof (b)); return b; }

static int streamRotation (AVStream* st)
{
    const uint8_t* sd = nullptr;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    if (auto* p = av_packet_side_data_get (st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX))
        sd = p->data;
#else
    sd = av_stream_get_side_data (st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
    if (sd == nullptr) return 0;
    double r = -av_display_rotation_get ((const int32_t*) sd);
    if (std::isnan (r)) return 0;
    int deg = ((int) std::lround (r) % 360 + 360) % 360;
    return (deg + 45) / 90 * 90 % 360;
}

bool isMediaFile (const juce::File& f)
{
    static const juce::StringArray ext { ".mp4", ".mov", ".m4v", ".mkv", ".avi", ".webm", ".mts", ".mxf",
                                         ".mp3", ".wav", ".flac", ".aif", ".aiff", ".m4a", ".aac", ".ogg",
                                         ".png", ".jpg", ".jpeg" };
    return ext.contains (f.getFileExtension().toLowerCase());
}

ProbeResult probeMedia (const juce::File& f)
{
    ProbeResult r;
    AVFormatContext* fmt = nullptr;
    int e = avformat_open_input (&fmt, f.getFullPathName().toRawUTF8(), nullptr, nullptr);
    if (e < 0) { r.error = "Cannot open: " + avErr (e); return r; }
    if ((e = avformat_find_stream_info (fmt, nullptr)) < 0) { r.error = avErr (e); avformat_close_input (&fmt); return r; }
    int vs = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int as = av_find_best_stream (fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    r.hasAudio = as >= 0;
    if (vs >= 0 && (fmt->streams[vs]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
    {
        auto* st = fmt->streams[vs];
        r.hasVideo = true;
        r.width = st->codecpar->width; r.height = st->codecpar->height;
        auto fr = av_guess_frame_rate (fmt, st, nullptr);
        r.fps = (fr.num > 0 && fr.den > 0) ? av_q2d (fr) : 30.0;
        r.rotation = streamRotation (st);
        bool still = juce::StringArray { ".png", ".jpg", ".jpeg" }.contains (f.getFileExtension().toLowerCase());
        r.kind = still ? MediaKind::Image : MediaKind::Video;
    }
    else if (r.hasAudio) r.kind = MediaKind::Audio;
    else { r.error = "No audio or video stream"; avformat_close_input (&fmt); return r; }

    if (fmt->duration > 0) r.duration = (Ticks) ((double) fmt->duration / AV_TIME_BASE * (double) kFlicks);
    if (r.kind == MediaKind::Image) r.duration = secToTicks (5.0);
    r.ok = r.duration > 0;
    if (! r.ok) r.error = "Unknown duration";
    avformat_close_input (&fmt);
    return r;
}

bool decodeAudio (const juce::File& f, int rate, int channels, std::vector<float>& out, juce::String& error)
{
    out.clear();
    AVFormatContext* fmt = nullptr;
    int e = avformat_open_input (&fmt, f.getFullPathName().toRawUTF8(), nullptr, nullptr);
    if (e < 0) { error = avErr (e); return false; }
    avformat_find_stream_info (fmt, nullptr);
    ISFXCodecPtr codec = nullptr;
    int si = av_find_best_stream (fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (si < 0 || codec == nullptr) { error = "No audio stream"; avformat_close_input (&fmt); return false; }
    auto* ctx = avcodec_alloc_context3 (codec);
    avcodec_parameters_to_context (ctx, fmt->streams[si]->codecpar);
    if ((e = avcodec_open2 (ctx, codec, nullptr)) < 0) { error = avErr (e); avcodec_free_context (&ctx); avformat_close_input (&fmt); return false; }

    SwrContext* swr = nullptr;
#if ISFX_NEW_CHLAYOUT
    AVChannelLayout outL; av_channel_layout_default (&outL, channels);
    if (ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default (&ctx->ch_layout, ctx->ch_layout.nb_channels);
    swr_alloc_set_opts2 (&swr, &outL, AV_SAMPLE_FMT_FLT, rate, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
#else
    auto inLayout = ctx->channel_layout ? ctx->channel_layout : (uint64_t) av_get_default_channel_layout (ctx->channels);
    swr = swr_alloc_set_opts (nullptr, av_get_default_channel_layout (channels), AV_SAMPLE_FMT_FLT, rate,
                              (int64_t) inLayout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
#endif
    if (swr == nullptr || swr_init (swr) < 0) { error = "Resampler init failed"; avcodec_free_context (&ctx); avformat_close_input (&fmt); return false; }

    auto* pkt = av_packet_alloc(); auto* fr = av_frame_alloc();
    std::vector<float> buf;
    auto drain = [&]
    {
        while (avcodec_receive_frame (ctx, fr) >= 0)
        {
            int maxOut = (int) av_rescale_rnd (swr_get_delay (swr, ctx->sample_rate) + fr->nb_samples, rate, ctx->sample_rate, AV_ROUND_UP);
            buf.resize ((size_t) (maxOut * channels));
            auto* o = (uint8_t*) buf.data();
            int n = swr_convert (swr, &o, maxOut, (const uint8_t**) fr->extended_data, fr->nb_samples);
            if (n > 0) out.insert (out.end(), buf.begin(), buf.begin() + n * channels);
        }
    };
    while (av_read_frame (fmt, pkt) >= 0)
    {
        if (pkt->stream_index == si && avcodec_send_packet (ctx, pkt) >= 0) drain();
        av_packet_unref (pkt);
    }
    avcodec_send_packet (ctx, nullptr); drain();
    {
        buf.resize ((size_t) (4096 * channels)); auto* o = (uint8_t*) buf.data();
        int n = swr_convert (swr, &o, 4096, nullptr, 0);
        if (n > 0) out.insert (out.end(), buf.begin(), buf.begin() + n * channels);
    }
    av_frame_free (&fr); av_packet_free (&pkt); swr_free (&swr);
    avcodec_free_context (&ctx); avformat_close_input (&fmt);
    if (out.empty()) { error = "Audio stream decoded to nothing"; return false; }
    return true;
}

// ------------------------------------------------------------------ VideoDecoder
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close()
{
    if (sws) { sws_freeContext (sws); sws = nullptr; }
    if (frame) av_frame_free (&frame);
    if (curAv) av_frame_free (&curAv);
    if (swAv) av_frame_free (&swAv);
    if (nextAv) av_frame_free (&nextAv);
    curPts = nextPts = -1; curConverted = false;
    if (pkt) av_packet_free (&pkt);
    if (dec) avcodec_free_context (&dec);
    if (hwDev) av_buffer_unref (&hwDev);
    hwFmt = -1;
    if (fmt) avformat_close_input (&fmt);
    cur = {}; next = {}; haveNext = false; eof = false; stream = -1;
}

// GPU decode: VideoToolbox on macOS, D3D11VA on Windows. ISFX_HWDEC=<ffmpeg device type> overrides, "none" disables.
// Any file or GPU the hardware can't handle falls back to the CPU decoder.
static const char* hwDeviceName()
{
    static const juce::String env = juce::SystemStats::getEnvironmentVariable ("ISFX_HWDEC", {});
    if (env.isNotEmpty()) return env == "none" ? nullptr : env.toRawUTF8();
   #if JUCE_MAC
    return "videotoolbox";
   #elif JUCE_WINDOWS
    return "d3d11va";
   #else
    return nullptr;
   #endif
}

static AVPixelFormat pickHwFormat (AVCodecContext* c, const AVPixelFormat* fmts)
{
    const int want = (int) (intptr_t) c->opaque;
    for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if ((int) *p == want) return *p;
    for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p)   // hardware refused this stream: first software format
        if (! (av_pix_fmt_desc_get (*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    return fmts[0];
}

bool VideoDecoder::open (const juce::File& f, int maxHeight, juce::String& error)
{
    close();
    maxH = maxHeight;
    int e = avformat_open_input (&fmt, f.getFullPathName().toRawUTF8(), nullptr, nullptr);
    if (e < 0) { error = avErr (e); fmt = nullptr; return false; }
    avformat_find_stream_info (fmt, nullptr);
    ISFXCodecPtr codec = nullptr;
    stream = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || codec == nullptr) { error = "No video stream"; close(); return false; }
    auto* st = fmt->streams[stream];
    dec = avcodec_alloc_context3 (codec);
    avcodec_parameters_to_context (dec, st->codecpar);
    if (auto* name = hwDeviceName())
    {
        const auto type = av_hwdevice_find_type_by_name (name);
        for (int i = 0; type != AV_HWDEVICE_TYPE_NONE; ++i)
        {
            auto* cfg = avcodec_get_hw_config (codec, i);
            if (cfg == nullptr) break;
            if (cfg->device_type == type && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            {
                if (av_hwdevice_ctx_create (&hwDev, type, nullptr, nullptr, 0) >= 0) hwFmt = (int) cfg->pix_fmt;
                break;
            }
        }
    }
    if (hwDev != nullptr)
    {
        dec->hw_device_ctx = av_buffer_ref (hwDev);
        dec->opaque = (void*) (intptr_t) hwFmt;
        dec->get_format = pickHwFormat;
        dec->thread_count = 1;   // the GPU decodes; CPU threads would only add buffered surfaces
    }
    else
    {
        dec->thread_count = juce::jlimit (1, 8, juce::SystemStats::getNumCpus());   // more threads = more frame buffers, little gain
        dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if ((e = avcodec_open2 (dec, codec, nullptr)) < 0 && hwDev != nullptr)
    {
        avcodec_free_context (&dec); av_buffer_unref (&hwDev); hwFmt = -1;
        dec = avcodec_alloc_context3 (codec);
        avcodec_parameters_to_context (dec, st->codecpar);
        dec->thread_count = juce::jlimit (1, 8, juce::SystemStats::getNumCpus());
        dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        e = avcodec_open2 (dec, codec, nullptr);
    }
    if (e < 0) { error = avErr (e); close(); return false; }
    tb = av_q2d (st->time_base);
    auto fr = av_guess_frame_rate (fmt, st, nullptr);
    frameRate = (fr.num > 0 && fr.den > 0) ? av_q2d (fr) : 30.0;
    rot = streamRotation (st);
    outW = dec->width; outH = dec->height;
    if (maxH > 0 && outH > maxH) { outW = (int) std::lround ((double) outW * maxH / outH) & ~1; outH = maxH; }
    frame = av_frame_alloc(); pkt = av_packet_alloc();
    curAv = av_frame_alloc(); nextAv = av_frame_alloc();
    fastScale = maxH > 0 && maxH <= 720;
    return true;
}

bool VideoDecoder::receive (AVFrame* into, Ticks& pts)
{
    while (true)
    {
        int r = avcodec_receive_frame (dec, into);
        if (r >= 0)
        {
            auto ts = into->best_effort_timestamp != AV_NOPTS_VALUE ? into->best_effort_timestamp : into->pts;
            auto startTs = fmt->streams[stream]->start_time != AV_NOPTS_VALUE ? fmt->streams[stream]->start_time : 0;
            pts = secToTicks ((double) (ts - startTs) * tb);
            return true;
        }
        if (r == AVERROR_EOF) { eof = true; return false; }
        int rr = av_read_frame (fmt, pkt);
        if (rr < 0) { avcodec_send_packet (dec, nullptr); continue; }
        if (pkt->stream_index == stream) avcodec_send_packet (dec, pkt);
        av_packet_unref (pkt);
    }
}

void VideoDecoder::convert (const AVFrame* src, Frame& out)
{
    if (src->hw_frames_ctx != nullptr)
    {
        if (swAv == nullptr) swAv = av_frame_alloc();
        av_frame_unref (swAv);
        if (av_hwframe_transfer_data (swAv, src, 0) < 0) { out.w = out.h = 0; return; }
        swAv->pts = src->pts;
        src = swAv;
    }
    // Preview tiers: plain area averaging (clean, ~3x cheaper). Export/full: accurate rounding + full chroma.
    const int flags = fastScale ? (outH < src->height ? SWS_AREA : SWS_BILINEAR)
                                : ((outH < src->height ? SWS_AREA : SWS_BICUBIC) | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT);
    sws = sws_getCachedContext (sws, src->width, src->height, (AVPixelFormat) src->format,
                                outW, outH, AV_PIX_FMT_RGBA, flags, nullptr, nullptr, nullptr);
    out.w = outW; out.h = outH; out.rgba.resize ((size_t) (outW * outH * 4));
    uint8_t* dst[1] = { out.rgba.data() }; int ls[1] = { outW * 4 };
    sws_scale (sws, src->data, src->linesize, 0, src->height, dst, ls);
}

bool VideoDecoder::decodeNext (Frame& out)
{
    Ticks pts = -1;
    if (! receive (frame, pts)) return false;
    convert (frame, out);
    out.pts = pts;
    return true;
}

bool VideoDecoder::seekTo (Ticks t)
{
    auto* st = fmt->streams[stream];
    auto startTs = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    int64_t ts = startTs + (int64_t) (ticksToSec (std::max<Ticks> (0, t)) / tb);
    if (av_seek_frame (fmt, stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers (dec);
    eof = false; haveNext = false; cur = {};
    curPts = nextPts = -1; curConverted = false;
    return true;
}

bool VideoDecoder::frameAt (Ticks t, Frame& out)
{
    auto* f = frameRef (t);
    if (f == nullptr) return false;
    out = *f;
    return true;
}

const Frame* VideoDecoder::frameRef (Ticks t)
{
    if (! isOpen()) return nullptr;
    const Ticks frameDur = secToTicks (1.0 / frameRate);
    bool backwards = curPts >= 0 && t < curPts;
    bool farAhead = curPts >= 0 && t > curPts + secToTicks (2.0);
    if (curPts < 0 || backwards || farAhead) seekTo (t);
    while (true)
    {
        if (! haveNext)
        {
            // Catching up (playback outran decode): don't decode B-frames nobody will see.
            dec->skip_frame = (curPts >= 0 && t - curPts > frameDur * 3) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (eof || ! receive (nextAv, nextPts)) break;
            haveNext = true;
        }
        if (nextPts <= t + frameDur / 4 || curPts < 0)
        {
            std::swap (curAv, nextAv); curPts = nextPts; haveNext = false; curConverted = false;
            if (curPts >= t) break;
        }
        else break;
    }
    dec->skip_frame = AVDISCARD_DEFAULT;
    if (curPts < 0) return nullptr;
    if (! curConverted) { convert (curAv, cur); cur.pts = curPts; curConverted = true; }
    return &cur;
}

PrefetchDecoder::PrefetchDecoder() : juce::Thread ("ISFX prefetch") {}
PrefetchDecoder::~PrefetchDecoder() { signalThreadShouldExit(); wake.signal(); stopThread (3000); }

bool PrefetchDecoder::open (const juce::File& f, int maxHeight, juce::String& error)
{
    if (! dec.open (f, maxHeight, error)) return false;
    rot = dec.rotation();
    frameDur = std::max<Ticks> (1, secToTicks (1.0 / std::max (1.0, dec.fps())));
    startThread();
    return true;
}

void PrefetchDecoder::prime (Ticks t)
{
    { const juce::ScopedLock sl (lock); target = std::max<Ticks> (0, t); }
    wake.signal();
}

std::shared_ptr<const Frame> PrefetchDecoder::get (Ticks t, int waitMs)
{
    prime (t);
    const double until = juce::Time::getMillisecondCounterHiRes() + waitMs;
    for (;;)
    {
        {
            const juce::ScopedLock sl (lock);
            std::shared_ptr<Frame> best;
            for (auto& f : q) { if (f->pts <= t + frameDur / 4) best = f; else break; }
            if (best == nullptr && ! q.empty() && q.front()->pts - t < frameDur * 2) best = q.front();
            if (best != nullptr) return best;
        }
        const double left = until - juce::Time::getMillisecondCounterHiRes();
        if (left <= 0) return nullptr;
        ready.wait ((int) std::ceil (left));
    }
}

void PrefetchDecoder::run()
{
    while (! threadShouldExit())
    {
        Ticks from = -1; bool reseek = false; std::shared_ptr<Frame> slot;
        {
            const juce::ScopedLock sl (lock);
            const Ticks t = target;
            if (t >= 0)
            {
                while (q.size() > 1 && q[1]->pts <= t) { pool.push_back (q.front()); q.pop_front(); }   // passed frames
                const bool jumped = q.empty() || t < q.front()->pts - frameDur || t > q.back()->pts + secToTicks (1.0);
                if (jumped) { for (auto& f : q) pool.push_back (f); q.clear(); atEnd = false; reseek = true; from = t; }
                else if (q.size() < maxAhead && ! atEnd) from = q.back()->pts + frameDur;
                if (from >= 0) { if (! pool.empty()) { slot = pool.back(); pool.pop_back(); } else slot = std::make_shared<Frame>(); }
            }
        }
        if (from < 0) { wake.wait (20); continue; }
        const Frame* fr = dec.frameRef (from);
        const bool ok = fr != nullptr && fr->valid();
        if (ok) { slot->w = fr->w; slot->h = fr->h; slot->pts = fr->pts; slot->rgba.assign (fr->rgba.begin(), fr->rgba.end()); }
        {
            const juce::ScopedLock sl (lock);
            if (ok && (reseek || q.empty() || slot->pts > q.back()->pts))
            {
                if (reseek && ! q.empty()) { for (auto& f : q) pool.push_back (f); q.clear(); }
                q.push_back (slot);
                maxAhead = (size_t) juce::jlimit (2, 6, (int) (48000000 / std::max<size_t> (1, slot->rgba.size())));
            }
            else { if (! reseek) atEnd = true; pool.push_back (slot); }
        }
        ready.signal();
    }
}

juce::File proxyFileFor (const juce::File& source, int height)
{
    auto key = source.getFullPathName() + "|" + juce::String (source.getSize()) + "|"
             + juce::String (source.getLastModificationTime().toMilliseconds()) + "|" + juce::String (height);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("HumHouse").getChildFile ("InSequence FX").getChildFile ("Proxies")
        .getChildFile (juce::String::toHexString (key.hashCode64()) + ".mkv");
}

bool makeProxy (const juce::File& source, const juce::File& dest, int height, double durationSec,
                const std::function<bool (double)>& progress, juce::String& error)
{
    VideoDecoder d;
    if (! d.open (source, height, error)) return false;
    const int W = d.width() & ~1, H = d.height() & ~1;
    dest.getParentDirectory().createDirectory();
    auto tmp = dest.getSiblingFile (dest.getFileNameWithoutExtension() + ".part.mkv");
    tmp.deleteFile();
    AVFormatContext* oc = nullptr;
    AVCodecContext* enc = nullptr;
    AVFrame* yuv = nullptr;
    AVPacket* pk = av_packet_alloc();
    SwsContext* sw = nullptr;
    bool ok = false;
    auto cleanup = [&]
    {
        if (oc != nullptr) { if (oc->pb != nullptr) avio_closep (&oc->pb); avformat_free_context (oc); }
        avcodec_free_context (&enc); av_frame_free (&yuv); av_packet_free (&pk); sws_freeContext (sw);
        if (! ok) tmp.deleteFile();
        return ok;
    };
    avformat_alloc_output_context2 (&oc, nullptr, "matroska", tmp.getFullPathName().toRawUTF8());
    auto* codec = avcodec_find_encoder (AV_CODEC_ID_MJPEG);
    if (oc == nullptr || codec == nullptr) { error = "MJPEG proxy encoder unavailable"; return cleanup(); }
    enc = avcodec_alloc_context3 (codec);
    enc->width = W; enc->height = H; enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->time_base = { 1, 1000 };
    enc->flags |= AV_CODEC_FLAG_QSCALE; enc->global_quality = FF_QP2LAMBDA * 3;
    enc->thread_count = 0;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int e = avcodec_open2 (enc, codec, nullptr);
    if (e < 0) { error = avErr (e); return cleanup(); }
    auto* st = avformat_new_stream (oc, nullptr);
    avcodec_parameters_from_context (st->codecpar, enc);
    st->time_base = { 1, 1000 };
    if ((e = avio_open (&oc->pb, tmp.getFullPathName().toRawUTF8(), AVIO_FLAG_WRITE)) < 0) { error = avErr (e); return cleanup(); }
    if ((e = avformat_write_header (oc, nullptr)) < 0) { error = avErr (e); return cleanup(); }
    yuv = av_frame_alloc(); yuv->format = enc->pix_fmt; yuv->width = W; yuv->height = H;
    av_frame_get_buffer (yuv, 0);
    sw = sws_getContext (d.width(), d.height(), AV_PIX_FMT_RGBA, W, H, AV_PIX_FMT_YUVJ420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
    auto drain = [&]
    {
        while (avcodec_receive_packet (enc, pk) >= 0)
        {
            av_packet_rescale_ts (pk, enc->time_base, st->time_base);
            pk->stream_index = st->index;
            av_interleaved_write_frame (oc, pk);
        }
    };
    Frame f; int64_t lastPts = -1;
    while (d.readNext (f))
    {
        int64_t pts = std::max (lastPts + 1, (int64_t) std::llround (ticksToSec (f.pts) * 1000.0));
        lastPts = pts;
        av_frame_make_writable (yuv);
        const uint8_t* src[1] = { f.rgba.data() }; int ls[1] = { f.w * 4 };
        sws_scale (sw, src, ls, 0, f.h, yuv->data, yuv->linesize);
        yuv->pts = pts;
        if (avcodec_send_frame (enc, yuv) < 0) { error = "proxy encode failed"; return cleanup(); }
        drain();
        if (progress && ! progress (durationSec > 0 ? pts / 1000.0 / durationSec : 0)) { error = "cancelled"; return cleanup(); }
    }
    avcodec_send_frame (enc, nullptr); drain();
    av_write_trailer (oc);
    ok = lastPts >= 0;
    if (! ok) error = "no frames decoded";
    cleanup();
    if (ok) { dest.deleteFile(); ok = tmp.moveFileTo (dest); }
    return ok;
}

juce::Image frameToImage (const Frame& f, int rotation)
{
    if (! f.valid()) return {};
    bool swapWH = rotation == 90 || rotation == 270;
    int W = swapWH ? f.h : f.w, H = swapWH ? f.w : f.h;
    juce::Image img (juce::Image::ARGB, W, H, false);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
        {
            auto* s = &f.rgba[(size_t) ((y * f.w + x) * 4)];
            int dx = x, dy = y;
            if (rotation == 90) { dx = f.h - 1 - y; dy = x; }
            else if (rotation == 180) { dx = f.w - 1 - x; dy = f.h - 1 - y; }
            else if (rotation == 270) { dx = y; dy = f.w - 1 - x; }
            bd.setPixelColour (dx, dy, juce::Colour (s[0], s[1], s[2], (juce::uint8) 255));
        }
    return img;
}

juce::Image makeThumbnail (const juce::File& f, Ticks t, int height)
{
    VideoDecoder d; juce::String err;
    if (! d.open (f, height, err)) return {};
    Frame fr;
    if (! d.frameAt (t, fr)) return {};
    return frameToImage (fr, d.rotation());
}

Waveform buildWaveform (const std::vector<float>& mono, int rate, double bps)
{
    Waveform w; w.bucketsPerSec = bps;
    size_t per = (size_t) std::max (1.0, rate / bps);
    w.peaks.resize (mono.size() / per + 1, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i)
        w.peaks[i / per] = std::max (w.peaks[i / per], std::abs (mono[i]));
    return w;
}
}
