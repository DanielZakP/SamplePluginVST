#include "Lyrics.h"

#include <juce_core/juce_core.h>
#include <whisper.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace choplab
{

const std::vector<LyricsModelInfo>& lyricsModels()
{
    static const std::vector<LyricsModelInfo> models {
        { "Fast (60 MB)", "ggml-base-q5_1.bin", 59 * 1024 * 1024, (int) WHISPER_AHEADS_BASE },
        { "Accurate (190 MB)", "ggml-small-q5_1.bin", 181 * 1024 * 1024, (int) WHISPER_AHEADS_SMALL },
    };
    return models;
}

juce::File lyricsModelFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ChopLab").getChildFile ("Models");
}

juce::File lyricsModelFile (int modelIndex)
{
    const auto& m = lyricsModels()[(size_t) juce::jlimit (0, (int) lyricsModels().size() - 1, modelIndex)];
    return lyricsModelFolder().getChildFile (m.fileName);
}

juce::URL lyricsModelUrl (int modelIndex)
{
    const auto& m = lyricsModels()[(size_t) juce::jlimit (0, (int) lyricsModels().size() - 1, modelIndex)];
    return juce::URL ("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + juce::String (m.fileName));
}

bool lyricsSupportedOnThisCpu()
{
   #if JUCE_INTEL
    // The speech engine is built for AVX2/FMA (any Intel since 2013, AMD since 2015).
    return juce::SystemStats::hasAVX2() && juce::SystemStats::hasFMA3();
   #else
    return true;
   #endif
}

const std::vector<LyricsLanguage>& lyricsLanguages()
{
    static const std::vector<LyricsLanguage> languages = []
    {
        std::vector<LyricsLanguage> v;
        for (int id = 0; id <= whisper_lang_max_id(); ++id)
        {
            const juce::String name (whisper_lang_str_full (id));
            v.push_back ({ whisper_lang_str (id), name.substring (0, 1).toUpperCase() + name.substring (1) });
        }
        return v;
    }();
    return languages;
}

juce::String lyricsLanguageName (const juce::String& code)
{
    for (const auto& l : lyricsLanguages())
        if (l.code == code)
            return l.name;
    return code;
}

//==============================================================================
bool downloadLyricsModel (int modelIndex, const LyricsProgress& progress, juce::String& error)
{
    const auto& info = lyricsModels()[(size_t) juce::jlimit (0, (int) lyricsModels().size() - 1, modelIndex)];
    const auto target = lyricsModelFile (modelIndex);
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    if (! target.getParentDirectory().createDirectory())
    {
        error = "Couldn't create " + target.getParentDirectory().getFullPathName();
        return false;
    }

    int status = 0;
    auto stream = lyricsModelUrl (modelIndex).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                     .withConnectionTimeoutMs (30000)
                                                                     .withNumRedirectsToFollow (10)
                                                                     .withStatusCode (&status));
    if (stream == nullptr || status >= 400)
    {
        error = "Couldn't download the lyrics model from huggingface.co" + (status >= 400 ? " (HTTP " + juce::String (status) + ")" : juce::String())
                + ". Check your internet connection.";
        return false;
    }

    const auto total = stream->getTotalLength();
    part.deleteFile();
    juce::int64 received = 0;
    {
        juce::FileOutputStream out (part);
        if (! out.openedOk())
        {
            error = "Couldn't write " + part.getFullPathName();
            return false;
        }

        juce::HeapBlock<char> buffer (1 << 16);
        for (;;)
        {
            const int n = stream->read (buffer, 1 << 16);
            if (n <= 0)
                break;
            if (! out.write (buffer, (size_t) n))
            {
                error = "Ran out of disk space while downloading the lyrics model";
                part.deleteFile();
                return false;
            }
            received += n;
            const float fraction = (float) received / (float) (total > 0 ? total : info.approxBytes);
            if (progress && ! progress (juce::jlimit (0.0f, 1.0f, fraction)))
            {
                out.flush();
                part.deleteFile();
                error = "Download cancelled";
                return false;
            }
        }
        out.flush();
    }

    if ((total > 0 && received != total) || received < info.approxBytes * 8 / 10)
    {
        part.deleteFile();
        error = "The lyrics model download was cut off. Try again.";
        return false;
    }

    target.deleteFile();
    if (! part.moveFileTo (target))
    {
        error = "Couldn't save " + target.getFullPathName();
        return false;
    }
    return true;
}

//==============================================================================
namespace
{
std::vector<float> toMono16k (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    constexpr double target = 16000.0;
    const int n = audio.getNumSamples();
    std::vector<float> mono ((size_t) n + 16, 0.0f);
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += audio.getSample (c, i) / (float) audio.getNumChannels();

    if (sampleRate > target)
        for (int stage = 0; stage < 3; ++stage)
        {
            juce::IIRFilter lp;
            lp.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, target * 0.45, 0.707));
            lp.processSamples (mono.data(), n);
        }

    const double ratio = sampleRate / target;
    std::vector<float> out ((size_t) juce::jmax (0, (int) std::floor (n / ratio)));
    juce::LagrangeInterpolator interp;
    interp.process (ratio, mono.data(), out.data(), (int) out.size());

    // Quiet vocals transcribe worse; bring the peak up to a healthy level.
    float peak = 0.0f;
    for (auto s : out)
        peak = std::max (peak, std::abs (s));
    if (peak > 1.0e-4f)
        for (auto& s : out)
            s *= 0.9f / peak;
    return out;
}

void silenceLogs()
{
    static std::once_flag once;
    std::call_once (once, []
    {
        whisper_log_set ([] (ggml_log_level, const char*, void*) {}, nullptr);
    });
}

juce::String plain (const juce::String& s)
{
    return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz ").trim();
}

// Speech models invent these on instrumentals and silence.
bool looksLikeHallucination (const juce::String& text, float avgProbability, float noSpeech)
{
    const auto t = text.trim();
    if (t.isEmpty() || t.startsWithChar ('[') || t.startsWithChar ('(') || t.startsWithChar ('*') || t.contains (juce::CharPointer_UTF8 ("\xe2\x99\xaa")))
        return true;

    static const char* const phrases[] { "thank you", "thanks for watching", "thank you for watching", "please subscribe",
                                         "subtitles by the amaraorg community", "you", "bye", "music" };
    const auto p = plain (t);
    const bool unsure = avgProbability < 0.55f || noSpeech > 0.4f;
    for (auto* phrase : phrases)
        if (p == phrase && unsure)
            return true;
    return false;
}

struct Abort
{
    const LyricsProgress* progress;
    std::atomic<float> fraction { 0.0f };
    std::atomic<bool> cancelled { false };
};
} // namespace

LyricsResult transcribeLyrics (const juce::AudioBuffer<float>& audio, double sampleRate, int modelIndex,
                               const juce::String& languageCode, const LyricsProgress& progress)
{
    LyricsResult result;
    silenceLogs();

    if (! lyricsSupportedOnThisCpu())
    {
        result.error = "Lyrics need a CPU with AVX2 (Intel 2013+ / AMD 2015+)";
        return result;
    }
    const auto modelFile = lyricsModelFile (modelIndex);
    if (! modelFile.existsAsFile())
    {
        result.error = "The lyrics model isn't downloaded yet";
        return result;
    }

    const auto pcm = toMono16k (audio, sampleRate);
    if (pcm.size() < 16000 / 2)
    {
        result.error = "Too short to find lyrics in";
        return result;
    }

    auto cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    cparams.flash_attn = false;
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset = (whisper_alignment_heads_preset) lyricsModels()[(size_t) modelIndex].alignmentPreset;

    auto* ctx = whisper_init_from_file_with_params (modelFile.getFullPathName().toRawUTF8(), cparams);
    if (ctx == nullptr)
    {
        modelFile.deleteFile();
        result.error = "The lyrics model file was damaged, so it was deleted. Click Find lyrics to download it again.";
        return result;
    }

    Abort abort { &progress };
    const std::string language = languageCode.isEmpty() ? "auto" : languageCode.toStdString();

    auto params = whisper_full_default_params (WHISPER_SAMPLING_BEAM_SEARCH);
    params.beam_search.beam_size = 5;
    params.n_threads = juce::jlimit (1, 8, juce::SystemStats::getNumCpus() - 1);
    params.print_progress = params.print_realtime = params.print_special = params.print_timestamps = false;
    params.token_timestamps = true;
    params.no_context = true;
    params.suppress_blank = true;
    params.suppress_nst = true;
    params.language = language.c_str();
    params.detect_language = false;
    params.progress_callback = [] (whisper_context*, whisper_state*, int percent, void* user)
    {
        auto& a = *static_cast<Abort*> (user);
        a.fraction = (float) percent / 100.0f;
        if (*a.progress && ! (*a.progress) (a.fraction))
            a.cancelled = true;
    };
    params.progress_callback_user_data = &abort;
    params.abort_callback = [] (void* user)
    {
        auto& a = *static_cast<Abort*> (user);
        if (*a.progress && ! (*a.progress) (a.fraction))
            a.cancelled = true;
        return a.cancelled.load();
    };
    params.abort_callback_user_data = &abort;

    const int rc = whisper_full (ctx, params, pcm.data(), (int) pcm.size());
    if (abort.cancelled)
    {
        whisper_free (ctx);
        result.cancelled = true;
        return result;
    }
    if (rc != 0)
    {
        whisper_free (ctx);
        result.error = "The speech engine failed (" + juce::String (rc) + ")";
        return result;
    }

    result.language = whisper_lang_str (whisper_full_lang_id (ctx));
    const auto eot = whisper_token_eot (ctx);

    for (int s = 0; s < whisper_full_n_segments (ctx); ++s)
    {
        const int nTokens = whisper_full_n_tokens (ctx, s);
        double pSum = 0.0;
        int pCount = 0;
        for (int t = 0; t < nTokens; ++t)
            if (whisper_full_get_token_id (ctx, s, t) < eot)
            {
                pSum += whisper_full_get_token_p (ctx, s, t);
                ++pCount;
            }
        const float avgP = pCount > 0 ? (float) (pSum / pCount) : 0.0f;
        const juce::String segmentText = juce::String::fromUTF8 (whisper_full_get_segment_text (ctx, s));
        if (looksLikeHallucination (segmentText, avgP, whisper_full_get_segment_no_speech_prob (ctx, s)))
            continue;

        // Tokens that begin with a space start a new word; the rest (including punctuation and
        // pieces of multi-byte characters) attach to the current one.
        std::string bytes;
        LyricWord word;
        double wordP = 0.0;
        int wordTokens = 0;
        auto flush = [&]
        {
            auto text = juce::String::fromUTF8 (bytes.data(), (int) bytes.size()).trim();
            if (text.containsAnyOf ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
                || (text.isNotEmpty() && (juce::juce_wchar) text[0] > 127))
            {
                word.text = text;
                word.probability = wordTokens > 0 ? (float) (wordP / wordTokens) : 0.0f;
                if (! (word.text.startsWithChar ('[') || word.text.startsWithChar ('(')))
                    result.words.push_back (word);
            }
            bytes.clear();
            wordP = 0.0;
            wordTokens = 0;
        };

        for (int t = 0; t < nTokens; ++t)
        {
            const auto data = whisper_full_get_token_data (ctx, s, t);
            if (data.id >= eot)
                continue;
            const char* piece = whisper_full_get_token_text (ctx, s, t);
            if (piece == nullptr || *piece == 0)
                continue;
            const bool startsWord = piece[0] == ' ' || bytes.empty();
            if (startsWord && ! bytes.empty())
                flush();
            if (bytes.empty())
            {
                const auto t0 = data.t_dtw >= 0 ? data.t_dtw : data.t0;
                word.start = (double) t0 / 100.0;
            }
            bytes += piece;
            word.end = (double) data.t1 / 100.0;
            wordP += data.p;
            ++wordTokens;
        }
        if (! bytes.empty())
            flush();
    }
    whisper_free (ctx);

    // Tidy timings: in order, no overlaps, every word at least 60 ms long.
    std::stable_sort (result.words.begin(), result.words.end(), [] (const LyricWord& a, const LyricWord& b) { return a.start < b.start; });
    const double length = (double) audio.getNumSamples() / sampleRate;
    for (size_t i = 0; i < result.words.size(); ++i)
    {
        auto& w = result.words[i];
        w.start = juce::jlimit (0.0, length, w.start);
        const double next = i + 1 < result.words.size() ? result.words[i + 1].start : length;
        w.end = juce::jlimit (w.start + 0.06, juce::jmax (w.start + 0.06, next), w.end);
    }
    return result;
}

juce::String wordsBetween (const std::vector<LyricWord>& words, double startSeconds, double endSeconds)
{
    juce::StringArray parts;
    for (const auto& w : words)
    {
        const double mid = 0.5 * (w.start + w.end);
        if (mid >= startSeconds && mid < endSeconds)
            parts.add (w.text);
    }
    return parts.joinIntoString (" ");
}

} // namespace choplab
