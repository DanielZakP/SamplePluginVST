#pragma once

#include "Analysis/Analysis.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

namespace choplab
{

enum class ChopMode
{
    transients = 0,
    grid,
    manual
};

// Per-chop settings the user edits.
struct SliceSettings
{
    juce::String label;
    float pitch = 0.0f;      // semitones
    float speed = 1.0f;      // playback rate: 2 = twice as fast (half as long)
    bool keepPitch = true;   // speed changes length only (stretch) vs tape-style (pitch follows speed)
    bool reverse = false;
    float gainDb = 0.0f;
    float attackMs = 0.0f;
    float releaseMs = 15.0f;
};

struct Slice
{
    juce::int64 start = 0;
    juce::int64 end = 0;
    SliceSettings settings;
    SliceDescription info;
};

// Settings that apply to the whole sample.
struct GlobalSettings
{
    float pitch = 0.0f;
    float speed = 1.0f;
    bool keepPitch = true;
    bool syncToHost = false; // stretch so the sample's BPM matches the project tempo
    float gainDb = 0.0f;
    int rootNote = 60;       // slice 1 plays on this MIDI note (60 = C5 in FL Studio)
    bool oneShot = false;    // play the whole chop regardless of note length
    bool mono = false;       // a new chop cuts off whatever is playing
};

struct ChopSettings
{
    ChopMode mode = ChopMode::transients;
    float sensitivity = 0.5f;  // 0..1
    double gridBeats = 1.0;    // grid chop size in quarter notes; 0 = one bar
    float minLengthMs = 70.0f; // transient chops closer than this are merged
};

// The loaded audio. Immutable once created, shared with the render thread.
struct SampleData
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::String name;
    juce::File file;

    double lengthSeconds() const { return audio.getNumSamples() / sampleRate; }
};

// Everything the editor shows. Owned and mutated on the message thread.
struct Document
{
    std::shared_ptr<const SampleData> sample;
    std::shared_ptr<const AnalysisFeatures> features;
    std::vector<OnsetCandidate> onsets;

    TempoResult detectedTempo;
    GridResult detectedGrid;
    KeyResult key;

    double bpm = 120.0;
    double downbeatSeconds = 0.0;
    TimeSignature timeSig;
    bool meterIsAuto = true;
    bool reversed = false;

    std::vector<Slice> slices;
    GlobalSettings global;
    ChopSettings chop;

    bool hasSample() const { return sample != nullptr && sample->audio.getNumSamples() > 0; }
    juce::int64 length() const { return sample != nullptr ? sample->audio.getNumSamples() : 0; }
    double sampleRate() const { return sample != nullptr ? sample->sampleRate : 44100.0; }
    double secondsAt (juce::int64 pos) const { return (double) pos / sampleRate(); }
    GridPosition gridPosition (juce::int64 pos) const { return gridPositionAt (secondsAt (pos), bpm, downbeatSeconds, timeSig); }
    double lengthInBeats (const Slice& s) const { return secondsAt (s.end - s.start) * bpm / 60.0; }
    int noteForSlice (int index) const
    {
        const int n = global.rootNote + index;
        return n <= 127 ? n : -1;
    }
};

constexpr int kMaxSlices = 128;

} // namespace choplab
