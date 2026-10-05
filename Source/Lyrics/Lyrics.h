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
constexpr LyricsTiming kDefaultLyricsTiming = LyricsTiming::alignment;

// Transcribes with word timestamps. languageCode empty = detect. Blocking.
LyricsResult transcribeLyrics (const juce::AudioBuffer<float>& audio, double sampleRate, int modelIndex,
                               const juce::String& languageCode, const LyricsProgress&,
                               LyricsTiming timing = kDefaultLyricsTiming);

// Words whose middle falls inside [startSeconds, endSeconds), joined with spaces.
juce::String wordsBetween (const std::vector<LyricWord>&, double startSeconds, double endSeconds);

} // namespace choplab
