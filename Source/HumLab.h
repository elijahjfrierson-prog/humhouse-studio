// HumLab: our own Edison-style audio editor. Offline edits on a stereo buffer; a <= b sample range, empty = whole buffer
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace hhs::humlab
{
bool cut (juce::AudioBuffer<float>&, int a, int b);        // removes the range, short crossfade at the join
bool trim (juce::AudioBuffer<float>&, int a, int b);       // keeps only the range
void silence (juce::AudioBuffer<float>&, int a, int b);
void fade (juce::AudioBuffer<float>&, int a, int b, bool fadeIn);
void normalize (juce::AudioBuffer<float>&, int a, int b, float peakDb = -0.3f);
void reverse (juce::AudioBuffer<float>&, int a, int b);
void gain (juce::AudioBuffer<float>&, int a, int b, float db);
void denoise (juce::AudioBuffer<float>&, double sr, int a, int b, float amount);   // spectral gate learned from the quietest frames
std::vector<float> spectrum (const juce::AudioBuffer<float>&, double sr, int a, int b, int bands);   // dB per log band, 30 Hz - 20 kHz
}
