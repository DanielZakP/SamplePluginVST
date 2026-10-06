#pragma once

#include "Analysis/Analysis.h"
#include "Lyrics/Lyrics.h"
#include "Stems/Stems.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
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
    juce::String lyrics;       // only used when lyricsEdited; otherwise the detected words are shown
    bool lyricsEdited = false;
    int stem = -1;             // play this stem of the chop (a StemKind), or -1 for whatever the sample plays
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

struct LyricsState
{
    std::vector<LyricWord> words;
    juce::String language;          // code of the language that was transcribed, e.g. "en"
    juce::String requestedLanguage; // empty = detect automatically
    int model = 0;                  // index into lyricsModels()
    bool searched = false;          // transcription has run on this sample
};

// A note in the built-in piano roll. Times are in quarter-note beats from the pattern start.
struct PatternNote
{
    int chop = 0;
    double start = 0.0;
    double length = 1.0;
    float velocity = 0.8f;
    double offset = 0.0; // seconds of the chop skipped when this note plays (it starts that far in)

    double end() const { return start + length; }
};

struct Pattern
{
    double lengthBeats = 16.0;
    double snapBeats = 0.25;
    std::vector<PatternNote> notes;
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

// Stems split from the mix. Same length, rate and direction (reversed or not) as the mix.
struct StemSet
{
    std::shared_ptr<const SampleData> mix;
    std::array<std::shared_ptr<const SampleData>, kNumStems> stems;
    std::array<juce::File, kNumStems> files; // the WAVs on disk (always the forward version)
    std::array<juce::int64, kNumStems> hashes {}; // of the audio in those files, to know them again
};

// Everything the editor shows. Owned and mutated on the message thread.
struct Document
{
    std::shared_ptr<const SampleData> sample; // what plays: the mix, or one of its stems
    std::shared_ptr<const StemSet> stems;     // null until the mix is separated
    int source = -1;                          // -1 = the mix, otherwise a StemKind
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
    LyricsState lyrics;
    Pattern pattern;

    bool hasSample() const { return sample != nullptr && sample->audio.getNumSamples() > 0; }
    // The full mix, whichever source is playing. Chops, lyrics and tempo belong to its timeline.
    std::shared_ptr<const SampleData> mix() const { return stems != nullptr ? stems->mix : sample; }
    // The stem a chop has been set to play, if there are stems; otherwise what the sample plays.
    int stemFor (const Slice& s) const { return stems != nullptr && s.settings.stem >= 0 && s.settings.stem < kNumStems ? s.settings.stem : -1; }
    std::shared_ptr<const SampleData> audioFor (const Slice& s) const
    {
        const int stem = stemFor (s);
        return stem >= 0 ? stems->stems[(size_t) stem] : sample;
    }
    juce::int64 length() const { return sample != nullptr ? sample->audio.getNumSamples() : 0; }
    double sampleRate() const { return sample != nullptr ? sample->sampleRate : 44100.0; }
    double secondsAt (juce::int64 pos) const { return (double) pos / sampleRate(); }
    GridPosition gridPosition (juce::int64 pos) const { return gridPositionAt (secondsAt (pos), bpm, downbeatSeconds, timeSig); }
    double lengthInBeats (const Slice& s) const { return secondsAt (s.end - s.start) * bpm / 60.0; }
    // What a chop says: the user's edit if there is one, otherwise the detected words inside it.
    juce::String lyricsFor (int index) const
    {
        if (index < 0 || index >= (int) slices.size())
            return {};
        const auto& s = slices[(size_t) index];
        if (s.settings.lyricsEdited)
            return s.settings.lyrics;
        return wordsBetween (lyrics.words, secondsAt (s.start), secondsAt (s.end));
    }

    int noteForSlice (int index) const
    {
        const int n = global.rootNote + index;
        return n <= 127 ? n : -1;
    }
};

constexpr int kMaxSlices = 128;

} // namespace choplab
