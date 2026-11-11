#include "App.h"

namespace isfx
{
void Player::play (Ticks from)
{
    startSample = (juce::int64) std::llround (ticksToSec (std::max<Ticks> (0, from)) * kMixRate);
    posSec = ticksToSec (std::max<Ticks> (0, from));
    restart = true;
    playing = true;
}

void Player::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int nOut, int n, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < nOut; ++c) if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);
    if (! playing) return;
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;
    if (restart.exchange (false))
    {
        fL.clear(); fR.clear(); readPos = 0;
        fifoStart = mixPos = startSample.load();
        rack.reset();
        if (trackFx) if (auto* tr = dynamic_cast<TrackRacks*> (trackFx)) tr->reset();
    }
    const double ratio = kMixRate / rate;
    const double need = readPos + n * ratio + 2;
    bL.resize (kBlock); bR.resize (kBlock);
    while ((double) fL.size() < need)
    {
        mixSequence (seq, st.audio, mixPos, kBlock, bL.data(), bR.data(), trackFx);
        rack.process (bL.data(), bR.data(), kBlock);
        fL.insert (fL.end(), bL.begin(), bL.end()); fR.insert (fR.end(), bR.begin(), bR.end());
        mixPos += kBlock;
    }
    for (int i = 0; i < n; ++i)
    {
        double s = readPos + i * ratio; auto i0 = (size_t) s; float f = (float) (s - (double) i0);
        float l = fL[i0] + (fL[i0 + 1] - fL[i0]) * f, r = fR[i0] + (fR[i0 + 1] - fR[i0]) * f;
        if (nOut > 0 && out[0]) out[0][i] = juce::jlimit (-1.0f, 1.0f, l);
        if (nOut > 1 && out[1]) out[1][i] = juce::jlimit (-1.0f, 1.0f, r);
    }
    readPos += n * ratio;
    auto drop = (size_t) readPos;
    fL.erase (fL.begin(), fL.begin() + (long) drop); fR.erase (fR.begin(), fR.begin() + (long) drop);
    fifoStart += (juce::int64) drop; readPos -= (double) drop;
    posSec = ((double) fifoStart + readPos) / kMixRate;
}
}
