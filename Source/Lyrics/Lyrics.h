#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <functional>
#include <vector>

namespace choplab
{

struct LyricWord
{
    juce::String text;
    double start = 0.0; // seconds
    double end = 0.0;
    float probability = 1.0f;
};

struct LyricsModelInfo
{
    const char* name;        // shown in the UI
    const char* fileName;    // on Hugging Face and on disk
    juce::int64 approxBytes;
    int alignmentPreset;     // whisper_alignment_heads_preset for word timing
};

// 0 = fast (base), 1 = accurate (small). Both multilingual, so any language can be detected.
const std::vector<LyricsModelInfo>& lyricsModels();
juce::File lyricsModelFolder();
juce::File lyricsModelFile (int modelIndex);
juce::URL lyricsModelUrl (int modelIndex);
bool lyricsSupportedOnThisCpu();

// Whisper's language list: codes ("en") and names ("english"), in Whisper's order.
struct LyricsLanguage
{
    juce::String code, name;
};
const std::vector<LyricsLanguage>& lyricsLanguages();
juce::String lyricsLanguageName (const juce::String& code); // "en" -> "English"

// Progress callbacks get 0..1 and return false to cancel.
using LyricsProgress = std::function<bool (float)>;

// Downloads a model to lyricsModelFile(). Blocking; call from a background thread.
bool downloadLyricsModel (int modelIndex, const LyricsProgress&, juce::String& error);

struct LyricsResult
{
    std::vector<LyricWord> words;
    juce::String language; // detected or forced code, e.g. "en"
    juce::String error;
    bool cancelled = false;
};

// How word start times are worked out (the tests compare them on speech with known timing).
enum class LyricsTiming
{
    alignment,  // cross-attention alignment (DTW) per token
    tokens,     // whisper's timestamp-token interpolation
    wordSegments // one segment per word
};
constexpr LyricsTiming kDefaultLyricsTiming = LyricsTiming::tokens;

// Transcribes with word timestamps. languageCode empty = detect. Blocking.
LyricsResult transcribeLyrics (const juce::AudioBuffer<float>& audio, double sampleRate, int modelIndex,
                               const juce::String& languageCode, const LyricsProgress&,
                               LyricsTiming timing = kDefaultLyricsTiming);

// Where words could start: sharp rises in the speech band.
struct SpeechOnset
{
    double time = 0.0;        // seconds
    double quietBefore = 0.0; // how long it was silent just before, in seconds
};
std::vector<SpeechOnset> speechOnsets (const juce::AudioBuffer<float>& audio, double sampleRate);

// Moves word starts onto nearby speech onsets (within maxShift seconds), keeping the words in
// order. Uses a global best match so one bad guess can't drag its neighbours onto the wrong onset.
// A word whose estimate lands in the silence before a phrase moves to where the phrase starts.
void snapWordsToOnsets (std::vector<LyricWord>& words, const std::vector<SpeechOnset>& onsets, double maxShift = 0.32);

// Words whose middle falls inside [startSeconds, endSeconds), joined with spaces.
juce::String wordsBetween (const std::vector<LyricWord>&, double startSeconds, double endSeconds);

} // namespace choplab
