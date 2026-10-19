#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #pragma comment (lib, "mfplat.lib")
 #pragma comment (lib, "mfreadwrite.lib")
 #pragma comment (lib, "mfuuid.lib")
 #pragma comment (lib, "ole32.lib")
#elif defined (__APPLE__)
 #include <AudioToolbox/AudioToolbox.h>
#endif
#include "AudioEncode.h"
#include <cstring>
#include <vector>
extern "C" {
#include <layer3.h>
}

namespace hhs
{
double compressedRate (double rate)
{
    if (std::abs (rate - 32000.0) < 1.0 || std::abs (rate - 44100.0) < 1.0 || std::abs (rate - 48000.0) < 1.0) return rate;
    return std::fmod (rate, 11025.0) < 1.0 ? 44100.0 : 48000.0;
}

namespace
{
struct Dither   // TPDF dither for the 16-bit encoder input
{
    juce::Random rng { 0x5eed };
    int16_t operator() (float x)
    {
        const float d = (rng.nextFloat() - rng.nextFloat()) / 32768.0f;
        return (int16_t) juce::jlimit (-32768, 32767, (int) std::lrint ((x + d) * 32767.0f));
    }
};
}

bool encodeMp3 (const juce::AudioBuffer<float>& b, double sr, const juce::File& out, int kbps)
{
    shine_config_t cfg {};
    shine_set_config_mpeg_defaults (&cfg.mpeg);
    cfg.wave.channels = PCM_STEREO;
    cfg.wave.samplerate = (int) std::lrint (sr);
    cfg.mpeg.mode = STEREO;
    cfg.mpeg.bitr = kbps;
    if (shine_check_config (cfg.wave.samplerate, kbps) < 0) return false;
    shine_t s = shine_initialise (&cfg);
    if (s == nullptr) return false;
    out.deleteFile();
    juce::FileOutputStream os (out);
    if (! os.openedOk()) { shine_close (s); return false; }
    const int spp = shine_samples_per_pass (s), len = b.getNumSamples();
    const int nch = b.getNumChannels();
    std::vector<int16_t> pcm ((size_t) spp * 2);
    Dither dither;
    int written = 0;
    for (int pos = 0; pos < len; pos += spp)
    {
        for (int i = 0; i < spp; ++i)
            for (int ch = 0; ch < 2; ++ch)
                pcm[(size_t) (2 * i + ch)] = pos + i < len ? dither (b.getSample (juce::jmin (ch, nch - 1), pos + i)) : 0;
        if (auto* d = shine_encode_buffer_interleaved (s, pcm.data(), &written)) os.write (d, (size_t) written);
    }
    if (auto* d = shine_flush (s, &written)) os.write (d, (size_t) written);
    shine_close (s);
    os.flush();
    return ! os.getStatus().failed();
}

bool encodeM4a (const juce::AudioBuffer<float>& b, double sr, const juce::File& out, int kbps)
{
    const int len = b.getNumSamples(), nch = b.getNumChannels();
    auto sample = [&] (int ch, int i) { return b.getSample (juce::jmin (ch, nch - 1), i); };
#if defined (__APPLE__)
    out.deleteFile();
    const auto path = out.getFullPathName();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) std::strlen (path.toRawUTF8()), false);
    if (url == nullptr) return false;
    AudioStreamBasicDescription dst {};
    dst.mSampleRate = sr; dst.mFormatID = kAudioFormatMPEG4AAC; dst.mChannelsPerFrame = 2;
    ExtAudioFileRef f = nullptr;
    OSStatus st = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &dst, nullptr, kAudioFileFlags_EraseFile, &f);
    CFRelease (url);
    if (st != noErr || f == nullptr) return false;
    AudioStreamBasicDescription src {};
    src.mSampleRate = sr; src.mFormatID = kAudioFormatLinearPCM;
    src.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    src.mChannelsPerFrame = 2; src.mBitsPerChannel = 32; src.mBytesPerFrame = 8; src.mFramesPerPacket = 1; src.mBytesPerPacket = 8;
    st = ExtAudioFileSetProperty (f, kExtAudioFileProperty_ClientDataFormat, sizeof (src), &src);
    AudioConverterRef conv = nullptr; UInt32 sz = sizeof (conv);
    if (st == noErr && ExtAudioFileGetProperty (f, kExtAudioFileProperty_AudioConverter, &sz, &conv) == noErr && conv != nullptr)
    {
        UInt32 br = (UInt32) kbps * 1000u;
        AudioConverterSetProperty (conv, kAudioConverterEncodeBitRate, sizeof (br), &br);
        CFArrayRef cc = nullptr;
        ExtAudioFileSetProperty (f, kExtAudioFileProperty_ConverterConfig, sizeof (cc), &cc);
    }
    std::vector<float> pcm (4096 * 2);
    for (int pos = 0; st == noErr && pos < len; pos += 4096)
    {
        const int n = juce::jmin (4096, len - pos);
        for (int i = 0; i < n; ++i) { pcm[(size_t) (2 * i)] = sample (0, pos + i); pcm[(size_t) (2 * i + 1)] = sample (1, pos + i); }
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = (UInt32) (n * 8);
        abl.mBuffers[0].mData = pcm.data();
        st = ExtAudioFileWrite (f, (UInt32) n, &abl);
    }
    return ExtAudioFileDispose (f) == noErr && st == noErr;
#elif defined (_WIN32)
    const bool coInit = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
    bool ok = SUCCEEDED (MFStartup (MF_VERSION));
    const bool mfUp = ok;
    out.deleteFile();
    IMFSinkWriter* w = nullptr; IMFMediaType* outT = nullptr; IMFMediaType* inT = nullptr; DWORD stream = 0;
    const UINT32 rate = (UINT32) std::lrint (sr);
    ok = ok && SUCCEEDED (MFCreateSinkWriterFromURL (out.getFullPathName().toWideCharPointer(), nullptr, nullptr, &w));
    ok = ok && SUCCEEDED (MFCreateMediaType (&outT));
    if (ok)
    {
        // the Windows AAC encoder takes 96/128/160/192 kbps (12000..24000 bytes/s)
        const UINT32 bytesPerSec = (UINT32) juce::jlimit (12000, 24000, kbps * 125 / 4000 * 4000);
        outT->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outT->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outT->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outT->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        outT->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
        outT->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
        ok = SUCCEEDED (w->AddStream (outT, &stream));
    }
    ok = ok && SUCCEEDED (MFCreateMediaType (&inT));
    if (ok)
    {
        inT->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inT->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inT->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inT->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        inT->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
        inT->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        inT->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
        ok = SUCCEEDED (w->SetInputMediaType (stream, inT, nullptr)) && SUCCEEDED (w->BeginWriting());
    }
    Dither dither;
    const int chunk = 4096;
    for (int pos = 0; ok && pos < len; pos += chunk)
    {
        const int n = juce::jmin (chunk, len - pos);
        IMFMediaBuffer* mb = nullptr; IMFSample* smp = nullptr; BYTE* p = nullptr;
        ok = SUCCEEDED (MFCreateMemoryBuffer ((DWORD) (n * 4), &mb)) && SUCCEEDED (mb->Lock (&p, nullptr, nullptr));
        if (ok)
        {
            auto* d = reinterpret_cast<int16_t*> (p);
            for (int i = 0; i < n; ++i) { d[2 * i] = dither (sample (0, pos + i)); d[2 * i + 1] = dither (sample (1, pos + i)); }
            mb->Unlock();
            mb->SetCurrentLength ((DWORD) (n * 4));
            ok = SUCCEEDED (MFCreateSample (&smp)) && SUCCEEDED (smp->AddBuffer (mb));
        }
        if (ok)
        {
            smp->SetSampleTime ((LONGLONG) pos * 10000000LL / (LONGLONG) rate);
            smp->SetSampleDuration ((LONGLONG) n * 10000000LL / (LONGLONG) rate);
            ok = SUCCEEDED (w->WriteSample (stream, smp));
        }
        if (smp) smp->Release();
        if (mb) mb->Release();
    }
    if (w) { if (ok) ok = SUCCEEDED (w->Finalize()); w->Release(); }
    if (inT) inT->Release();
    if (outT) outT->Release();
    if (mfUp) MFShutdown();
    if (coInit) CoUninitialize();
    return ok;
#else
    juce::ignoreUnused (len, sample, sr, out, kbps);
    return false;   // no system AAC encoder on this platform
#endif
}
}
