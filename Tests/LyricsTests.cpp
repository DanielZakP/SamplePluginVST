// Lyrics (speech-to-text) tests. They need a downloaded model, so they're run separately:
//   ChopLabTests --download-model 0        downloads the fast model the same way the plugin does
//   ChopLabTests --lyrics-test Tests/fixtures
//   ChopLabTests --lyrics file.wav [language] [model]

#include "Lyrics/Lyrics.h"
#include "TestSignals.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

using namespace choplab;

namespace
{
int lyricFailures = 0;

void expect (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (! ok)
        ++lyricFailures;
}

bool readWav (const juce::File& f, juce::AudioBuffer<float>& out, double& rate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr)
        return false;
    out.setSize ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&out, 0, (int) r->lengthInSamples, 0, true, true);
    rate = r->sampleRate;
    return true;
}

LyricsResult run (const juce::AudioBuffer<float>& audio, double rate, const juce::String& lang, int model,
                  LyricsTiming timing = kDefaultLyricsTiming)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = transcribeLyrics (audio, rate, model, lang, [] (float) { return true; }, timing);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    std::cout << "  language " << r.language << ", " << r.words.size() << " words in " << juce::String ((t1 - t0) / 1000.0, 1) << " s"
              << (r.error.isNotEmpty() ? ", error: " + r.error : juce::String()) << "\n   ";
    for (const auto& w : r.words)
        std::cout << " [" << juce::String (w.start, 2) << " " << w.text << "]";
    std::cout << "\n";
    return r;
}

juce::String norm (const juce::String& s) { return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz"); }

const LyricWord* find (const LyricsResult& r, const juce::String& word)
{
    for (const auto& w : r.words)
        if (norm (w.text) == word)
            return &w;
    return nullptr;
}

int countFound (const LyricsResult& r, std::initializer_list<const char*> words)
{
    int n = 0;
    for (auto* w : words)
        if (find (r, w) != nullptr)
            ++n;
    return n;
}
} // namespace

int downloadModelCommand (int model)
{
    std::cout << "Downloading " << lyricsModels()[(size_t) model].fileName << " to " << lyricsModelFile (model).getFullPathName() << "\n";
    juce::String error;
    int lastPercent = -10;
    const bool ok = downloadLyricsModel (model, [&] (float f)
    {
        const int percent = (int) (f * 100.0f);
        if (percent >= lastPercent + 10)
        {
            std::cout << "  " << percent << "%\n";
            lastPercent = percent;
        }
        return true;
    }, error);
    std::cout << (ok ? "done, " + juce::String (lyricsModelFile (model).getSize()) + " bytes" : "FAILED: " + error) << "\n";
    return ok ? 0 : 1;
}

int printLyricsCommand (const juce::File& file, const juce::String& lang, int model)
{
    juce::AudioBuffer<float> audio;
    double rate = 0;
    if (! readWav (file, audio, rate))
        return 1;
    run (audio, rate, lang, model);
    return 0;
}

int lyricsTestCommand (const juce::File& fixtures, int model)
{
    std::cout << "Lyrics tests with " << lyricsModels()[(size_t) model].name << ", CPU supported: " << (int) lyricsSupportedOnThisCpu() << "\n";
    expect (lyricsLanguages().size() > 90 && lyricsLanguageName ("es") == "Spanish", "language list loads (" + juce::String (lyricsLanguages().size()) + ")");

    juce::AudioBuffer<float> en, es;
    double enRate = 0, esRate = 0;
    expect (readWav (fixtures.getChildFile ("speech_en.wav"), en, enRate) && readWav (fixtures.getChildFile ("speech_es.wav"), es, esRate),
            "fixtures load");

    std::cout << "English speech, language auto-detected\n";
    auto r = run (en, enRate, {}, model);
    expect (r.language == "en", "detected English");
    expect (countFound (r, { "hello", "world", "test", "chop", "samples", "tonight" }) >= 4, "found most of the words");
    // Phrases start at 0.5 s, 2.5 s and 4.8 s
    auto near = [&] (const char* word, double t)
    {
        const auto* w = find (r, word);
        expect (w != nullptr && std::abs (w->start - t) < 0.45,
                juce::String ("\"") + word + "\" placed near " + juce::String (t, 1) + " s" + (w != nullptr ? " (got " + juce::String (w->start, 2) + ")" : juce::String()));
    };
    near ("hello", 0.5);
    near ("this", 2.5);
    near ("we", 4.8);
    expect (wordsBetween (r.words, 0.0, 2.0).toLowerCase().contains ("hello"), "words are assigned to the chop they fall in");
    expect (! wordsBetween (r.words, 2.0, 7.5).toLowerCase().contains ("hello"), "and not to other chops");

    // espeak's Spanish is robotic and the fast model mishears some of it, so ask for a few words
    std::cout << "Spanish speech, language auto-detected\n";
    auto rs = run (es, esRate, {}, model);
    expect (rs.language == "es", "detected Spanish");
    expect (countFound (rs, { "hola", "mundo", "esta", "es", "una", "prueba", "de", "la", "letra" }) >= 3, "found Spanish words");

    std::cout << "Spanish speech, language set by the user\n";
    auto rf = run (es, esRate, "es", model);
    expect (rf.language == "es" && countFound (rf, { "hola", "mundo", "esta", "es", "una", "prueba", "de", "la", "letra" }) >= 3,
            "forced language works");

    // Word timing: each word was placed at a known time
    std::cout << "Word timing on words with known start times\n";
    juce::AudioBuffer<float> wordsAudio;
    double wordsRate = 0;
    readWav (fixtures.getChildFile ("words_en.wav"), wordsAudio, wordsRate);
    juce::StringArray truthLines;
    truthLines.addLines (fixtures.getChildFile ("words_en.txt").loadFileAsString());
    struct Mode
    {
        LyricsTiming timing;
        const char* name;
    };
    for (auto mode : { Mode { LyricsTiming::alignment, "alignment (DTW)" }, Mode { LyricsTiming::tokens, "timestamp tokens" },
                       Mode { LyricsTiming::wordSegments, "one segment per word" } })
    {
        std::cout << " " << mode.name << ":\n";
        auto rw = run (wordsAudio, wordsRate, "en", model, mode.timing);
        double sumErr = 0.0, maxErr = 0.0;
        int matched = 0;
        for (const auto& line : truthLines)
        {
            const auto word = line.upToFirstOccurrenceOf (" ", false, false);
            const double truth = line.fromFirstOccurrenceOf (" ", false, false).getDoubleValue();
            if (const auto* w = find (rw, word))
            {
                const double err = w->start - truth;
                sumErr += std::abs (err);
                maxErr = std::max (maxErr, std::abs (err));
                ++matched;
            }
        }
        std::cout << "    matched " << matched << "/" << truthLines.size() << ", mean error " << juce::String (matched ? sumErr / matched : 0.0, 3)
                  << " s, worst " << juce::String (maxErr, 3) << " s" << (mode.timing == kDefaultLyricsTiming ? "  <- in use" : "") << "\n";
        if (mode.timing == kDefaultLyricsTiming)
        {
            expect (matched >= 7, "most words recognised");
            expect (matched > 0 && sumErr / matched < 0.1 && maxErr < 0.3, "word start times are close");
        }
    }

    std::cout << "Drum loop, no vocals\n";
    const double beat = 60.0 / 93.0;
    Synth drums (16 * beat);
    for (int b = 0; b < 4; ++b)
        drumBar (drums, b * 4 * beat, beat);
    auto rd = run (drums.buf, kRate, {}, model);
    expect (rd.words.size() <= 2, "no invented lyrics on drums (" + juce::String (rd.words.size()) + " words)");

    std::cout << "English speech over drums\n";
    juce::AudioBuffer<float> mix (1, (int) (en.getNumSamples() * kRate / enRate));
    mix.clear();
    {
        juce::LagrangeInterpolator li;
        std::vector<float> src ((size_t) en.getNumSamples() + 8, 0.0f);
        std::copy (en.getReadPointer (0), en.getReadPointer (0) + en.getNumSamples(), src.begin());
        li.process (enRate / kRate, src.data(), mix.getWritePointer (0), mix.getNumSamples());
        Synth bed (mix.getNumSamples() / kRate + 0.1);
        for (int b = 0; b * 4 * beat < mix.getNumSamples() / kRate; ++b)
            drumBar (bed, b * 4 * beat, beat);
        mix.addFrom (0, 0, bed.buf, 0, 0, mix.getNumSamples(), 0.3f);
    }
    auto rm = run (mix, kRate, {}, model);
    expect (countFound (rm, { "hello", "world", "test", "chop", "samples", "tonight" }) >= 3, "still finds words over a beat");

    std::cout << "\n" << (lyricFailures == 0 ? "ALL PASSED" : juce::String (lyricFailures) + " FAILED") << "\n";
    return lyricFailures == 0 ? 0 : 1;
}

// No model needed: whisper's raw word times on words_en.wav (recorded from a CI run of the fast
// model) are snapped to the speech onsets and compared with the true word starts.
int lyricsSnapTest (const juce::File& fixtures)
{
    std::cout << "Lyric word snapping (recorded whisper timings)\n";
    juce::AudioBuffer<float> audio;
    double rate = 0;
    if (! readWav (fixtures.getChildFile ("words_en.wav"), audio, rate))
    {
        expect (false, "words_en.wav loads");
        return 1;
    }
    const auto onsets = speechOnsets (audio, rate);
    std::cout << "  speech onsets:";
    for (auto o : onsets)
        std::cout << " " << juce::String (o.time, 2);
    std::cout << "\n";

    const std::vector<std::pair<const char*, double>> raw { { "Hello", 0.51 }, { "world,", 0.80 }, { "this", 1.92 }, { "is", 2.56 },
                                                            { "a", 2.88 },     { "test,", 3.04 },  { "we", 3.84 },   { "chop", 4.06 },
                                                            { "samples", 4.51 }, { "to", 5.30 },   { "hide.", 5.52 } };
    std::vector<LyricWord> words;
    for (const auto& r : raw)
        words.push_back ({ r.first, r.second, r.second + 0.2, 1.0f });
    snapWordsToOnsets (words, onsets);

    juce::StringArray truthLines;
    truthLines.addLines (fixtures.getChildFile ("words_en.txt").loadFileAsString());
    double sumBefore = 0, sumAfter = 0, worstAfter = 0;
    int n = 0;
    for (const auto& line : truthLines)
    {
        const auto word = line.upToFirstOccurrenceOf (" ", false, false);
        const double truth = line.fromFirstOccurrenceOf (" ", false, false).getDoubleValue();
        for (size_t i = 0; i < words.size(); ++i)
            if (norm (words[i].text) == word)
            {
                sumBefore += std::abs (raw[i].second - truth);
                const double err = std::abs (words[i].start - truth);
                sumAfter += err;
                worstAfter = std::max (worstAfter, err);
                ++n;
                std::cout << "  " << word << ": " << juce::String (raw[i].second, 2) << " -> " << juce::String (words[i].start, 2)
                          << " (actual " << juce::String (truth, 2) << ")\n";
            }
    }
    std::cout << "  mean error " << juce::String (sumBefore / juce::jmax (1, n), 3) << " s -> " << juce::String (sumAfter / juce::jmax (1, n), 3)
              << " s, worst " << juce::String (worstAfter, 3) << " s\n";
    expect (n >= 8 && sumAfter / n < 0.06 && worstAfter < 0.2, "snapping lands words on their real starts");
    bool ordered = true;
    for (size_t i = 1; i < words.size(); ++i)
        ordered = ordered && words[i].start > words[i - 1].start;
    expect (ordered, "words stay in order");

    // Whisper sometimes starts a phrase's first word in the pause before it (seen on Windows:
    // "We" at 4.00 s while the phrase starts at 4.79 s in speech_en.wav).
    juce::AudioBuffer<float> phrases;
    double phrasesRate = 0;
    readWav (fixtures.getChildFile ("speech_en.wav"), phrases, phrasesRate);
    std::vector<LyricWord> early { { "test", 3.40, 3.9, 1.0f }, { "We", 4.00, 4.3, 1.0f }, { "chop", 4.79, 5.0, 1.0f } };
    early[2].start = 5.32;
    snapWordsToOnsets (early, speechOnsets (phrases, phrasesRate));
    expect (std::abs (early[1].start - 4.79) < 0.06, "a word guessed into the pause moves to the phrase start (got " + juce::String (early[1].start, 2) + ")");

    // Nothing to snap to: estimates are left alone
    std::vector<LyricWord> untouched { { "a", 1.0, 1.2, 1.0f }, { "b", 2.0, 2.2, 1.0f } };
    snapWordsToOnsets (untouched, { { 5.0, 0.0 }, { 6.0, 0.0 } });
    expect (std::abs (untouched[0].start - 1.0) < 1.0e-9 && std::abs (untouched[1].start - 2.0) < 1.0e-9, "far-away onsets are ignored");
    return lyricFailures;
}
