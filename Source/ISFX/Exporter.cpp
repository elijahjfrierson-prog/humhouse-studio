#include "Exporter.h"
#include <future>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
 #define ISFX_NEW_CHLAYOUT 1
#else
 #define ISFX_NEW_CHLAYOUT 0
#endif

namespace isfx
{
static juce::String avErr (int e) { char b[256] = {}; av_strerror (e, b, sizeof (b)); return b; }

VideoEncoder::~VideoEncoder()
{
    if (sws) sws_freeContext (sws);
    if (vfr) av_frame_free (&vfr);
    if (afr) av_frame_free (&afr);
    if (pkt) av_packet_free (&pkt);
    if (venc) avcodec_free_context (&venc);
    if (aenc) avcodec_free_context (&aenc);
    if (fmt) { if (fmt->pb) avio_closep (&fmt->pb); avformat_free_context (fmt); }
}

bool exportCodecAvailable (const juce::String& codec)
{
    auto has = [] (std::initializer_list<const char*> n) { for (auto* x : n) if (avcodec_find_encoder_by_name (x) != nullptr) return true; return false; };
    if (codec == "h265") return has ({ "hevc_videotoolbox", "libx265", "hevc_nvenc", "hevc_qsv", "hevc_mf", "hevc_amf" });
    if (codec == "h265_lossless") return has ({ "libx265" });
    if (codec == "h264_lossless") return has ({ "libx264" });
    if (codec.startsWith ("prores")) return has ({ "prores_ks", "prores_videotoolbox", "prores_aw", "prores" });
    return true;
}

static AVPixelFormat pickPix (const AVCodec* c, std::initializer_list<AVPixelFormat> pref)
{
    if (c->pix_fmts == nullptr) return *pref.begin();
    for (auto p : pref) for (auto* q = c->pix_fmts; *q != AV_PIX_FMT_NONE; ++q) if (*q == p) return p;
    return c->pix_fmts[0];
}

bool VideoEncoder::open (const juce::File& f, int w, int h, double fps, int mbps, bool withAudio, juce::String& error, const juce::String& codec)
{
    W = w & ~1; H = h & ~1;
    auto path = f.getFullPathName();
    const bool mov = codec != "h264";
    if (avformat_alloc_output_context2 (&fmt, nullptr, mov ? "mov" : "mp4", path.toRawUTF8()) < 0 || fmt == nullptr) { error = "Cannot create output file"; return false; }
    AVRational fr = av_d2q (fps, 100000);
    std::vector<const char*> names;
    if (codec == "h265") names = { "hevc_videotoolbox", "libx265", "hevc_nvenc", "hevc_qsv", "hevc_mf", "hevc_amf" };
    else if (codec == "h265_lossless") names = { "libx265" };
    else if (codec == "h264_lossless") names = { "libx264" };
    else if (codec == "prores_hq" || codec == "prores_4444") names = { "prores_ks", "prores_videotoolbox", "prores_aw", "prores" };
    else names = { "h264_videotoolbox", "h264_mf", "libopenh264", "libx264", "h264_nvenc", "h264_qsv", "mpeg4" };
    for (auto* nm : names)
    {
        auto* c = avcodec_find_encoder_by_name (nm);
        if (c == nullptr) continue;
        const juce::String n (nm);
        venc = avcodec_alloc_context3 (c);
        venc->width = W; venc->height = H;
        venc->time_base = av_inv_q (fr); venc->framerate = fr;
        venc->pix_fmt = (c->pix_fmts && c->pix_fmts[0] == AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        if (n == "h264_videotoolbox") venc->pix_fmt = AV_PIX_FMT_NV12;
        venc->bit_rate = (int64_t) mbps * 1000000; venc->gop_size = (int) std::lround (fps * 2);
        venc->max_b_frames = 0;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (n == "libx264" && codec == "h264") av_opt_set (venc->priv_data, "preset", "fast", 0);
        if (n == "mpeg4") { venc->qmax = 6; venc->bit_rate = (int64_t) mbps * 1500000; }
        if (codec == "h264_lossless")
        {
            venc->pix_fmt = pickPix (c, { AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P }); venc->bit_rate = 0;
            av_opt_set (venc->priv_data, "preset", "veryfast", 0); av_opt_set (venc->priv_data, "qp", "0", 0);
        }
        if (codec.startsWith ("h265"))
        {
            venc->codec_tag = MKTAG ('h', 'v', 'c', '1');
            venc->pix_fmt = pickPix (c, codec == "h265_lossless" ? std::initializer_list<AVPixelFormat> { AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P }
                                                                 : std::initializer_list<AVPixelFormat> { AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P });
            if (n == "libx265")
            {
                venc->bit_rate = 0; av_opt_set (venc->priv_data, "preset", "fast", 0);
                av_opt_set (venc->priv_data, "x265-params", codec == "h265_lossless" ? "lossless=1:log-level=error" : "crf=10:log-level=error", 0);
            }
            else venc->bit_rate = (int64_t) mbps * 2000000;
        }
        if (codec.startsWith ("prores"))
        {
            const bool p4 = codec == "prores_4444";
            venc->bit_rate = 0;
            venc->pix_fmt = pickPix (c, p4 ? std::initializer_list<AVPixelFormat> { AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUVA444P10LE, AV_PIX_FMT_YUV422P10LE }
                                           : std::initializer_list<AVPixelFormat> { AV_PIX_FMT_YUV422P10LE });
            av_opt_set (venc->priv_data, "profile", p4 ? "4444" : "hq", 0);
        }
        if (avcodec_open2 (venc, c, nullptr) >= 0) { vcodecName = nm; break; }
        avcodec_free_context (&venc);
    }
    if (venc == nullptr) { error = "No " + codec + " encoder available on this computer"; return false; }
    vst = avformat_new_stream (fmt, nullptr);
    vst->time_base = venc->time_base;
    avcodec_parameters_from_context (vst->codecpar, venc);

    if (withAudio)
    {
        auto* c = avcodec_find_encoder (mov ? AV_CODEC_ID_PCM_S24LE : AV_CODEC_ID_AAC);
        if (c == nullptr) { error = mov ? "No PCM encoder" : "No AAC encoder"; return false; }
        aenc = avcodec_alloc_context3 (c);
        aenc->sample_rate = kMixRate; aenc->sample_fmt = mov ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_FLTP; aenc->bit_rate = mov ? 0 : 320000;
#if ISFX_NEW_CHLAYOUT
        av_channel_layout_default (&aenc->ch_layout, 2);
#else
        aenc->channels = 2; aenc->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        aenc->time_base = { 1, kMixRate };
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        int e = avcodec_open2 (aenc, c, nullptr);
        if (e < 0) { error = "AAC: " + avErr (e); return false; }
        ast = avformat_new_stream (fmt, nullptr);
        ast->time_base = aenc->time_base;
        avcodec_parameters_from_context (ast->codecpar, aenc);
        afr = av_frame_alloc();
        afr->format = aenc->sample_fmt; afr->nb_samples = aenc->frame_size > 0 ? aenc->frame_size : 1024; afr->sample_rate = kMixRate;
#if ISFX_NEW_CHLAYOUT
        av_channel_layout_copy (&afr->ch_layout, &aenc->ch_layout);
#else
        afr->channels = 2; afr->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        av_frame_get_buffer (afr, 0);
    }
    if (! (fmt->oformat->flags & AVFMT_NOFILE))
    {
        int e = avio_open (&fmt->pb, path.toRawUTF8(), AVIO_FLAG_WRITE);
        if (e < 0) { error = "Cannot write file: " + avErr (e); return false; }
    }
    int e = avformat_write_header (fmt, nullptr);
    if (e < 0) { error = "MP4 header: " + avErr (e); return false; }
    headerWritten = true;
    vfr = av_frame_alloc(); vfr->format = venc->pix_fmt; vfr->width = W; vfr->height = H;
    av_frame_get_buffer (vfr, 0);
    pkt = av_packet_alloc();
    sws = sws_getContext (w, h, AV_PIX_FMT_RGBA, W, H, venc->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
    return sws != nullptr;
}

bool VideoEncoder::encode (AVCodecContext* c, AVStream* st, AVFrame* f)
{
    if (avcodec_send_frame (c, f) < 0) return false;
    while (true)
    {
        int r = avcodec_receive_packet (c, pkt);
        if (r == AVERROR (EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) return false;
        av_packet_rescale_ts (pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;
        if (av_interleaved_write_frame (fmt, pkt) < 0) return false;
    }
}

bool VideoEncoder::writeVideo (const juce::uint8* rgba, juce::int64 idx)
{
    av_frame_make_writable (vfr);
    const uint8_t* src[1] = { rgba }; int ls[1] = { W * 4 };
    sws_scale (sws, src, ls, 0, H, vfr->data, vfr->linesize);
    vfr->pts = idx;
    return encode (venc, vst, vfr);
}

bool VideoEncoder::flushAudio (bool final)
{
    const int fs = afr->nb_samples;
    size_t pos = 0;
    while (pendL.size() - pos >= (size_t) fs || (final && pos < pendL.size()))
    {
        av_frame_make_writable (afr);
        size_t n = std::min ((size_t) fs, pendL.size() - pos);
        if (afr->format == AV_SAMPLE_FMT_S32)
        {
            auto* d = (int32_t*) afr->data[0];
            auto q = [] (float v) { return (int32_t) std::lround (juce::jlimit (-1.0f, 1.0f, v) * 2147483392.0); };
            for (size_t i = 0; i < (size_t) fs; ++i) { d[2 * i] = i < n ? q (pendL[pos + i]) : 0; d[2 * i + 1] = i < n ? q (pendR[pos + i]) : 0; }
        }
        else
        {
            auto* l = (float*) afr->data[0]; auto* r = (float*) afr->data[1];
            for (size_t i = 0; i < (size_t) fs; ++i) { l[i] = i < n ? pendL[pos + i] : 0.0f; r[i] = i < n ? pendR[pos + i] : 0.0f; }
        }
        afr->pts = audioPts; audioPts += fs;
        if (! encode (aenc, ast, afr)) return false;
        pos += n;
    }
    pendL.erase (pendL.begin(), pendL.begin() + (long) pos);
    pendR.erase (pendR.begin(), pendR.begin() + (long) pos);
    return true;
}

bool VideoEncoder::writeAudio (const float* L, const float* R, int n)
{
    if (aenc == nullptr) return true;
    pendL.insert (pendL.end(), L, L + n); pendR.insert (pendR.end(), R, R + n);
    return flushAudio (false);
}

bool VideoEncoder::finish (juce::String& error)
{
    if (! headerWritten) { error = "Encoder not open"; return false; }
    if (aenc) { flushAudio (true); encode (aenc, ast, nullptr); }
    encode (venc, vst, nullptr);
    int e = av_write_trailer (fmt);
    if (e < 0) { error = avErr (e); return false; }
    return true;
}

bool runExport (const Sequence& s, const AudioCache& audio, const ExportJob& job, FrameRenderFn render,
                ProgressFn progress, std::atomic<bool>& cancel, juce::String& error, juce::String* codecUsed)
{
    Ticks a = job.rangeStart, b = job.rangeEnd > 0 ? job.rangeEnd : s.duration();
    if (b <= a) { error = "Nothing to export (empty sequence)"; return false; }
    auto tmp = job.output.getSiblingFile (job.output.getFileNameWithoutExtension() + ".part" + job.output.getFileExtension());
    {
        VideoEncoder enc;
        if (! enc.open (tmp, job.width, job.height, job.fps, job.mbps, true, error, job.codec)) { tmp.deleteFile(); return false; }
        if (codecUsed) *codecUsed = enc.codecName();
        const juce::int64 frames = (juce::int64) std::ceil (ticksToSec (b - a) * job.fps - 1e-9);
        std::vector<juce::uint8> bufs[2] = { std::vector<juce::uint8> ((size_t) (job.width * job.height * 4)),
                                             std::vector<juce::uint8> ((size_t) (job.width * job.height * 4)) };
        std::future<bool> pend;
        std::vector<float> L, R;
        const juce::int64 aStart = (juce::int64) std::llround (ticksToSec (a) * kMixRate);
        juce::int64 audioDone = 0;
        for (juce::int64 f = 0; f < frames; ++f)
        {
            if (cancel.load()) { if (pend.valid()) pend.wait(); error = "Export cancelled"; enc.finish (error); tmp.deleteFile(); error = "Export cancelled"; return false; }
            Ticks t = a + frameToTicks (f, job.fps);   // exact frame time, no accumulation
            auto& buf = bufs[f & 1];
            if (! render (t, buf)) { if (pend.valid()) pend.wait(); error = "Render failed at frame " + juce::String (f); tmp.deleteFile(); return false; }
            if (pend.valid() && ! pend.get()) { error = "Video encode failed"; tmp.deleteFile(); return false; }
            const auto* data = buf.data();
            pend = std::async (std::launch::async, [&, f, data]
            {
                if (! enc.writeVideo (data, f)) return false;
                juce::int64 audioTarget = (juce::int64) std::llround ((double) (f + 1) / job.fps * kMixRate);
                int n = (int) (audioTarget - audioDone);
                if (n > 0)
                {
                    L.resize ((size_t) n); R.resize ((size_t) n);
                    mixSequence (s, audio, aStart + audioDone, n, L.data(), R.data(), job.trackFx);
                    if (job.audioFx) job.audioFx (L.data(), R.data(), n);
                    enc.writeAudio (L.data(), R.data(), n);
                    audioDone = audioTarget;
                }
                return true;
            });
            if (progress && (f % 5 == 0 || f == frames - 1)) progress ((double) (f + 1) / (double) frames);
        }
        if (pend.valid() && ! pend.get()) { error = "Video encode failed"; tmp.deleteFile(); return false; }
        if (! enc.finish (error)) { tmp.deleteFile(); return false; }
    }
    job.output.deleteFile();
    if (! tmp.moveFileTo (job.output)) { error = "Could not move export into place"; return false; }
    return true;
}
}
