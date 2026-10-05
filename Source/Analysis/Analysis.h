#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>

namespace choplab
{

using Chroma = std::array<float, 12>;

// Intermediate spectral data computed once per sample. Everything else
// (tempo, grid, key, onsets, per-slice info) is derived from this, so user
// corrections like "BPM x2" or "this is 3/4" can be re-evaluated instantly.
struct AnalysisFeatures
{
    double sourceRate = 44100.0;
    juce::int64 sourceLength = 0;

    double analysisRate = 22050.0;
    std::vector<float> mono;              // mono mix at analysisRate

    // Onset envelopes, one value per hop (frame t is centred at t * hop / analysisRate)
    int hop = 256;
    double fps = 0.0;
    std::vector<float> onsetEnv;          // full-band spectral flux
    std::vector<float> lowEnv;            // flux below ~150 Hz (kicks, 808s)
    std::vector<float> midEnv;            // flux 150 Hz - 5 kHz (snares, claps, most instruments)
    std::vector<float> highEnv;           // flux above ~5 kHz (hats)
    std::vector<float> frameDb;           // frame level, for gating silence

    // Harmonic content, one value per chroma hop
    int chromaHop = 1024;
    double chromaFps = 0.0;
    std::vector<Chroma> chroma;           // raw (unnormalised) pitch-class energy per frame
    std::vector<float> harmonicRatio;     // harmonic energy / total energy per frame
    std::vector<float> strongestPitch;    // MIDI pitch of the loudest harmonic peak, or -1
    double tuningSemitones = 0.0;         // estimated deviation from A440

    double durationSeconds() const { return sourceRate > 0 ? (double) sourceLength / sourceRate : 0.0; }
};

struct TempoResult
{
    double bpm = 120.0;
    float confidence = 0.0f;   // 0..1
    bool loopDetected = false; // sample length is a whole number of bars at this tempo
};

struct TimeSignature
{
    int numerator = 4;
    int denominator = 4;

    double quarterBeatsPerBar() const { return numerator * 4.0 / denominator; }
    juce::String toString() const { return juce::String (numerator) + "/" + juce::String (denominator); }
    bool operator== (const TimeSignature& o) const { return numerator == o.numerator && denominator == o.denominator; }
    bool operator!= (const TimeSignature& o) const { return ! (*this == o); }
};

struct GridResult
{
    double downbeatSeconds = 0.0;   // time of a bar start (first one at or after the sample start)
    TimeSignature timeSig;
    float meterConfidence = 0.0f;   // 0..1, how sure the 3/4 vs 4/4 call is
};

struct KeyGuess
{
    int tonic = 0;      // pitch class, 0 = C
    bool minor = false;
    float score = 0.0f; // profile correlation
};

struct KeyResult
{
    bool hasKey = false;      // false for drum loops and other non-tonal material
    KeyGuess best, alt;
    float confidence = 0.0f;  // 0..1
    Chroma profile {};        // normalised overall chroma
};

struct OnsetCandidate
{
    juce::int64 position = 0; // in source samples, already nudged to just before the attack
    float strength = 0.0f;    // 0..1, relative prominence
};

struct SliceDescription
{
    float peakDb = -100.0f;
    float rmsDb = -100.0f;
    juce::String type;      // "Kick", "Snare/Clap", "Hi-hat", "Bass", "Tonal", "Mixed", "Silence"
    juce::String harmony;   // chord ("Am"), single note ("C#5") or empty
};

// Position of a moment in the bar grid, e.g. bar 3, beat 2, "&".
struct GridPosition
{
    int bar = 1;            // 0 = pickup before the first downbeat
    int beat = 1;           // 1-based, in units of the time signature denominator
    int sixteenth = 1;      // 1-based sixteenth within the beat
    bool onGrid = false;    // within tolerance of a 16th line
    double offsetMs = 0.0;  // distance from the nearest 16th line
    juce::String describe;  // "Bar start", "Beat 3", "3 &", "3 e", "3 a", "Off grid"
};

// Typical order: computeFeatures -> detectOnsets -> estimateTempo -> estimateGrid, estimateKey.
AnalysisFeatures computeFeatures (const juce::AudioBuffer<float>& source, double sampleRate);
std::vector<OnsetCandidate> detectOnsets (const AnalysisFeatures&, const juce::AudioBuffer<float>& source);
TempoResult estimateTempo (const AnalysisFeatures&, const std::vector<OnsetCandidate>& onsets);
GridResult estimateGrid (const AnalysisFeatures&, const std::vector<OnsetCandidate>& onsets, double bpm,
                         TimeSignature forcedTimeSig, bool autoMeter, bool loopHint);
KeyResult estimateKey (const AnalysisFeatures&);
SliceDescription describeSlice (const AnalysisFeatures&, const juce::AudioBuffer<float>& source, juce::int64 start, juce::int64 end);

GridPosition gridPositionAt (double seconds, double bpm, double downbeatSeconds, TimeSignature);
juce::String keyName (const KeyGuess&);
juce::String pitchClassName (int pc);
juce::String midiNoteName (int note); // FL Studio convention: middle C (60) is C5

} // namespace choplab
