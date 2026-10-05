#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <functional>

namespace choplab
{

// Stem separation with Demucs v4 (htdemucs), run on the CPU through demucs.cpp.
enum StemKind
{
    stemVocals = 0,
    stemDrums,
    stemBass,
    stemOther,
    stemInstrumental, // drums + bass + other: everything but the vocals
    kNumStems
};
juce::String stemName (int stem); // "Vocals", "Drums", ...

juce::File stemsModelFile();
juce::URL stemsModelUrl();
bool stemsModelLooksComplete(); // there, and the size it should be
bool stemsSupportedOnThisCpu();

// Progress callbacks get 0..1 and return false to cancel.
using StemsProgress = std::function<bool (float)>;

// Downloads the model to stemsModelFile(). Blocking; call from a background thread.
bool downloadStemsModel (const StemsProgress&, juce::String& error);

// Separation threads worth running here for this much audio (samples at 44.1 kHz): one per
// physical core but one, and only as many as fit in memory (each needs about 1.5 GB while it works).
int stemWorkerCount (juce::int64 numSamples = 0);

struct StemsResult
{
    std::array<juce::AudioBuffer<float>, kNumStems> stems; // same rate, length and channel count as the input
    juce::String error;
    bool cancelled = false;
};

// Blocking and slow: several seconds of CPU time per second of audio, spread over `workers`
// threads (0 = stemWorkerCount()). Call from a background thread. modelFile defaults to stemsModelFile().
StemsResult separateStems (const juce::AudioBuffer<float>& audio, double sampleRate, const StemsProgress&, int workers = 0,
                           const juce::File& modelFile = {});

} // namespace choplab
