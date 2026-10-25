#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <vector>

namespace hhs
{
// HumHouse Stem Splitter: Drums, Bass, Vocals, Melodies from a finished mix. Offline (call off the audio thread).
// The masks add up to 1 in every bin and the last stem is the rest, so the stems sum back to the original exactly.
enum Stem { StemDrums = 0, StemBass, StemVocals, StemMelodies, NumStems };
const char* stemName (int stem);
// stem sets: Main (4 above), Drum parts (Kick, Snare & Clap, Hats & Cymbals, Percs, Music),
// Melody parts (Bass, Lead & Vocal, Keys & Plucks, Pads, Drums), Full (every part: 8 stems)
enum StemSet { StemSetMain = 0, StemSetDrumParts, StemSetMelodyParts, StemSetFull, NumStemSets };
int stemCount (int set);
const char* stemName (int set, int stem);
const char* stemSetLabel (int set);
juce::uint32 stemColour (int set, int stem);
// splits `src` (stereo or mono) at `sampleRate`; progress 0..1, cancel stops early (returns false)
bool splitStems (const juce::AudioBuffer<float>& src, double sampleRate, std::array<juce::AudioBuffer<float>, NumStems>& out,
                 std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
bool splitStems (const juce::AudioBuffer<float>& src, double sampleRate, int set, std::vector<juce::AudioBuffer<float>>& out,
                 std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
// file version: writes "<name> - Drums.wav" ... into outDir (32-bit float). Returns the files, empty on failure.
std::array<juce::File, NumStems> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir,
                                                    std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
std::vector<juce::File> splitStemsToFiles (const juce::File& in, double startSec, double lengthSec, const juce::File& outDir, int set,
                                           std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);
}
