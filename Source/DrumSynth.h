#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// Built-in synthesized kit so the app makes beats with zero samples installed.
namespace hhs
{
enum BuiltinDrum { Kick = 0, Clap, Snare, Hat, OpenHat, Perc, Rim, Tom, NumBuiltinDrums };
const char* builtinDrumName (int);
juce::AudioBuffer<float> renderBuiltinDrum (int which, double sampleRate);
}
