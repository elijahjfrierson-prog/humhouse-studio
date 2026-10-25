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
// HumTime slices: a piece of the source (src0..src1) placed at dst in the output, optionally through a Glitch Gate effect
struct Slice { int src0 = 0, src1 = 0, dst = 0, glitch = -1; bool reversed = false, selected = false; };
juce::AudioBuffer<float> glitchSlice (const juce::AudioBuffer<float>& seg, double sr, double bpm, double startBeat, int fx);
juce::AudioBuffer<float> arrange (const juce::AudioBuffer<float>& src, const std::vector<Slice>&, int minLen, double sr, double bpm, double startBeat);
std::vector<float> spectrum (const juce::AudioBuffer<float>&, double sr, int a, int b, int bands);   // dB per log band, 30 Hz - 20 kHz
}
