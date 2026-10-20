#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>

namespace hhs
{
// HumHouse Stem Splitter: Drums, Bass, Vocals, Melodies from a finished mix. Offline (call off the audio thread).
// The 4 masks add up to 1 in every bin, so the stems sum back to the original exactly.
enum Stem { StemDrums = 0, StemBass, StemVocals, StemMelodies, NumStems };
const char* stemName (int stem);
// splits `src` (stereo or mono) at `sampleRate`; progress 0..1, cancel stops early (returns false)
bool splitStems (const juce::AudioBuffer<float>& src, double sampleRate, std::array<juce::AudioBuffer<float>, NumStems>& out,
                 std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
// file version: writes "<name> - Drums.wav" ... into outDir (32-bit float). Returns the 4 files, empty on failure.
std::array<juce::File, NumStems> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir,
                                                    std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
}
