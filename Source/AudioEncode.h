#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace hhs
{
// compressed export: stereo float buffer -> file. Return false if the format/rate isn't supported here.
bool encodeMp3 (const juce::AudioBuffer<float>& buf, double sampleRate, const juce::File& out, int kbps = 320);
bool encodeM4a (const juce::AudioBuffer<float>& buf, double sampleRate, const juce::File& out, int kbps = 256);
// MP3/AAC only take 32/44.1/48 kHz: map any export rate onto the closest family
double compressedRate (double rate);
}
