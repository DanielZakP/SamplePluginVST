#include "Lyrics.h"

#include "Common/Download.h"

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
    return downloadFile (lyricsModelUrl (modelIndex), lyricsModelFile (modelIndex), info.approxBytes, "the lyrics model", progress, error);
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
                               const juce::String& languageCode, const LyricsProgress& progress, LyricsTiming timing)
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
    if (timing == LyricsTiming::wordSegments)
    {
        params.max_len = 1;
        params.split_on_word = true;
    }
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
                auto t0 = data.t0;
                if (timing == LyricsTiming::alignment && data.t_dtw >= 0)
                    t0 = data.t_dtw;
                else if (timing == LyricsTiming::wordSegments)
                    t0 = whisper_full_get_segment_t0 (ctx, s);
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

    // Whisper's times wander by up to ~0.3 s; pull each word onto the nearby moment it actually starts.
    // The words stay in whisper's text order, which is always right; only their times move.
    snapWordsToOnsets (result.words, speechOnsets (audio, sampleRate));

    // Tidy timings: no overlaps, every word at least 60 ms long.
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

//==============================================================================
namespace choplab
{

std::vector<SpeechOnset> speechOnsets (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::vector<SpeechOnset> onsets;
    const int n = audio.getNumSamples();
    if (n == 0 || sampleRate <= 0.0)
        return onsets;

    // Speech band only, so bass and cymbals matter less
    std::vector<float> x ((size_t) n, 0.0f);
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            x[(size_t) i] += audio.getSample (c, i);
    for (int stage = 0; stage < 2; ++stage)
    {
        juce::IIRFilter hp, lp;
        hp.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, 150.0, 0.707));
        lp.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (4000.0, sampleRate * 0.45), 0.707));
        hp.processSamples (x.data(), n);
        lp.processSamples (x.data(), n);
    }

    // Level in dB, 20 ms windows every 5 ms
    const int hop = juce::jmax (1, (int) (sampleRate * 0.005));
    const int win = hop * 4;
    std::vector<float> db;
    for (int start = 0; start + win <= n; start += hop)
    {
        double e = 0.0;
        for (int i = start; i < start + win; ++i)
            e += (double) x[(size_t) i] * x[(size_t) i];
        db.push_back ((float) (10.0 * std::log10 (e / win + 1.0e-12)));
    }
    if (db.size() < 10)
        return onsets;
    const float peak = *std::max_element (db.begin(), db.end());
    const float floorDb = juce::jmax (-55.0f, peak - 40.0f);

    // A word starts where the level climbs 9 dB or more above the recent minimum
    double lastOnset = -1.0;
    for (size_t k = 8; k < db.size(); ++k)
    {
        float recentMin = db[k - 1];
        size_t minAt = k - 1;
        for (size_t j = k - 8; j < k; ++j)
            if (db[j] < recentMin)
            {
                recentMin = db[j];
                minAt = j;
            }
        if (db[k] < floorDb || db[k] - recentMin < 9.0f)
            continue;
        // the onset is where it first rises 3 dB above that minimum
        size_t at = minAt;
        while (at < k && db[at] < recentMin + 3.0f)
            ++at;
        const double t = (double) at * hop / sampleRate + (double) win / sampleRate * 0.5;
        if (t - lastOnset > 0.08)
        {
            int quietFrames = 0;
            for (auto j = (std::ptrdiff_t) at - 1; j >= 0 && db[(size_t) j] < peak - 30.0f; --j)
                ++quietFrames;
            onsets.push_back ({ t, quietFrames * (double) hop / sampleRate });
            lastOnset = t;
        }
    }
    return onsets;
}

void snapWordsToOnsets (std::vector<LyricWord>& words, const std::vector<SpeechOnset>& onsets, double maxShift)
{
    const size_t W = words.size(), O = onsets.size();
    if (W == 0 || O == 0)
        return;

    // cost[i][j]: best total for the first i words using onsets before j. A word can keep its own
    // estimate (cost = keepPenalty) or take a later onset within maxShift (cost = distance).
    const double keepPenalty = maxShift * 1.2;
    const double inf = 1.0e18;
    std::vector<std::vector<double>> cost (W + 1, std::vector<double> (O + 1, inf));
    std::vector<std::vector<int>> choice (W + 1, std::vector<int> (O + 1, -2));
    for (size_t j = 0; j <= O; ++j)
        cost[0][j] = 0.0;

    std::vector<double> prefixMin (O + 1);
    for (size_t i = 1; i <= W; ++i)
    {
        const double est = words[i - 1].start;
        // prefixMin[j] = min of cost[i-1][0..j-1]
        prefixMin[0] = inf;
        for (size_t j = 1; j <= O; ++j)
            prefixMin[j] = std::min (prefixMin[j - 1], cost[i - 1][j - 1]);
        for (size_t j = 0; j <= O; ++j)
        {
            // keep the estimate; onsets before j are still unused
            double best = cost[i - 1][j] + keepPenalty;
            int pick = -1;
            // or take onset j-1, if it's after whatever the previous words used
            if (j > 0)
            {
                const auto& onset = onsets[j - 1];
                double d = std::abs (onset.time - est);
                // An estimate sitting in the silence before a phrase belongs to the phrase's start.
                const bool inSilenceBefore = onset.quietBefore >= 0.15 && est < onset.time && onset.time - est <= onset.quietBefore + 0.05;
                if (inSilenceBefore)
                    d = juce::jmin (d, 0.02);
                if (d <= maxShift)
                {
                    const double prev = prefixMin[j];
                    if (prev + d < best)
                    {
                        best = prev + d;
                        pick = (int) j - 1;
                    }
                }
                // or leave onset j-1 unused
                if (cost[i][j - 1] < best)
                {
                    best = cost[i][j - 1];
                    pick = -3;
                }
            }
            cost[i][j] = best;
            choice[i][j] = pick;
        }
    }

    // Walk back through the choices
    size_t j = O;
    for (size_t i = W; i > 0;)
    {
        const int c = choice[i][j];
        if (c == -3)
        {
            --j;
            continue;
        }
        if (c >= 0)
        {
            words[i - 1].start = onsets[(size_t) c].time;
            // the previous word must use an onset before this one
            size_t bestJ = 0;
            double bestCost = inf;
            for (size_t jj = 0; jj <= (size_t) c; ++jj)
                if (cost[i - 1][jj] < bestCost)
                {
                    bestCost = cost[i - 1][jj];
                    bestJ = jj;
                }
            j = bestJ;
        }
        --i;
    }

    // A word that kept its own estimate can end up before a neighbour that moved forward (e.g. out of
    // a pause). Words never swap: push it to the next onset after the previous word, or just after it.
    for (size_t w = 1; w < W; ++w)
    {
        auto& word = words[w];
        const double prev = words[w - 1].start;
        if (word.start > prev + 0.02)
            continue;
        double next = prev + 0.05;
        for (const auto& o : onsets)
            if (o.time > prev + 0.02 && o.time < prev + 1.0)
            {
                next = o.time;
                break;
            }
        word.start = next;
    }
}

} // namespace choplab
