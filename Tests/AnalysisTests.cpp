// Synthetic-signal tests for the analysis engine. Builds drum and chord loops with
// known tempo, key, meter and hit positions, runs the analysis, and checks the answers.
// Pass audio file paths as arguments to print the analysis of real files instead.

#include "Analysis/Analysis.h"
#include "Engine/Chopper.h"
#include "TestSignals.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <cmath>
#include <iostream>
#include <random>

using namespace choplab;

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (! ok)
        ++failures;
}

struct Analysis
{
    AnalysisFeatures features;
    TempoResult tempo;
    GridResult grid;
    KeyResult key;
    std::vector<OnsetCandidate> onsets;
};

Analysis analyse (const juce::AudioBuffer<float>& b, double rate)
{
    Analysis a;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    a.features = computeFeatures (b, rate);
    a.onsets = detectOnsets (a.features, b);
    a.tempo = estimateTempo (a.features, a.onsets);
    a.grid = estimateGrid (a.features, a.onsets, a.tempo.bpm, { 4, 4 }, true, a.tempo.loopDetected);
    a.key = estimateKey (a.features);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();

    std::cout << "  bpm " << a.tempo.bpm << " (conf " << a.tempo.confidence << (a.tempo.loopDetected ? ", loop" : "") << ")"
              << "  meter " << a.grid.timeSig.toString() << " (conf " << a.grid.meterConfidence << ")"
              << "  downbeat " << a.grid.downbeatSeconds << "s"
              << "  key " << (a.key.hasKey ? keyName (a.key.best) + " / alt " + keyName (a.key.alt) : juce::String ("none"))
              << " (score " << a.key.best.score << ", conf " << a.key.confidence << ")"
              << "  onsets " << a.onsets.size()
              << "  [" << juce::String (t1 - t0, 0) << " ms for " << juce::String (b.getNumSamples() / rate, 1) << " s]\n";
    return a;
}

bool bpmMatches (double got, double want, double tol = 0.05) { return std::abs (got - want) <= tol; }

void testDrumLoop (double bpm, int bars)
{
    std::cout << "Drum loop, " << bpm << " BPM, " << bars << " bars\n";
    const double beat = 60.0 / bpm;
    Synth s (bars * 4 * beat);
    for (int b = 0; b < bars; ++b)
        drumBar (s, b * 4 * beat, beat);

    const auto a = analyse (s.buf, kRate);
    check (bpmMatches (a.tempo.bpm, bpm) || bpmMatches (a.tempo.bpm, bpm * 2) || bpmMatches (a.tempo.bpm, bpm / 2),
           "tempo is " + juce::String (bpm) + " (or x2 / half)");
    if (bpmMatches (a.tempo.bpm, bpm))
    {
        check (true, "tempo exactly " + juce::String (bpm));
        check (a.grid.timeSig == TimeSignature { 4, 4 }, "meter 4/4");
        check (std::abs (a.grid.downbeatSeconds) < 0.02, "downbeat at 0");
    }
    else
    {
        // Half/double-time is genuinely ambiguous; the plugin has x2 and /2 buttons. Check that the
        // grid comes out right once the user corrects it.
        std::cout << "  [NOTE] got " << a.tempo.bpm << " instead of " << bpm << " (half/double ambiguity)\n";
        const auto g = estimateGrid (a.features, a.onsets, bpm, { 4, 4 }, true, true);
        check (g.timeSig == TimeSignature { 4, 4 }, "after correcting the tempo: meter 4/4");
        check (std::abs (g.downbeatSeconds) < 0.02, "after correcting the tempo: downbeat at 0 (got " + juce::String (g.downbeatSeconds, 3) + ")");
    }
    check (! a.key.hasKey, "no key reported for a drum loop");

    // Onset accuracy: every hit found, slightly early is fine, late is not.
    std::vector<double> hits = s.hitTimes;
    std::sort (hits.begin(), hits.end());
    hits.erase (std::unique (hits.begin(), hits.end(), [] (double x, double y) { return std::abs (x - y) < 0.001; }), hits.end());
    int found = 0;
    double worstEarly = 0, worstLate = 0;
    for (double h : hits)
    {
        double bestErr = 1e9;
        for (const auto& o : a.onsets)
        {
            const double err = (double) o.position / kRate - h;
            if (std::abs (err) < std::abs (bestErr))
                bestErr = err;
        }
        if (std::abs (bestErr) < 0.02)
        {
            ++found;
            worstEarly = std::min (worstEarly, bestErr);
            worstLate = std::max (worstLate, bestErr);
        }
    }
    std::cout << "  onsets matched " << found << "/" << hits.size() << ", error range " << worstEarly * 1000.0 << " .. "
              << worstLate * 1000.0 << " ms\n";
    check (found == (int) hits.size(), "every hit detected");
    check (worstLate <= 0.0015, "no slice starts more than 1.5 ms after its hit");
    check (worstEarly >= -0.006, "no slice starts more than 6 ms early");
}

void testChordLoop (double bpm, bool withDrums)
{
    std::cout << "A minor chord loop (Am F C G), " << bpm << " BPM" << (withDrums ? " with drums" : "") << "\n";
    const double beat = 60.0 / bpm;
    const int bars = 4;
    Synth s (bars * 4 * beat);
    const std::vector<std::vector<int>> chords { { 57, 60, 64, 45 }, { 53, 57, 60, 41 }, { 48, 52, 55, 60 }, { 55, 59, 62, 43 } };
    for (int b = 0; b < bars; ++b)
    {
        const auto& c = chords[(size_t) b];
        for (int n : c)
            s.note (b * 4 * beat, 4 * beat, n);
        // melody
        s.note (b * 4 * beat + 2 * beat, beat, c[0] + 12, 0.08f);
        if (withDrums)
            drumBar (s, b * 4 * beat, beat);
    }

    const auto a = analyse (s.buf, kRate);
    check (a.key.hasKey, "a key is reported");
    const bool aMinor = a.key.best.tonic == 9 && a.key.best.minor;
    const bool cMajor = a.key.best.tonic == 0 && ! a.key.best.minor;
    check (aMinor || cMajor, "key is A minor (or its relative, C major)");
    check (aMinor || (a.key.alt.tonic == 9 && a.key.alt.minor), "A minor is the top or alternate guess");
    if (! withDrums)
    {
        // 4 chords + 4 melody notes = 8 real onsets
        const auto markers = transientMarkers (a.onsets, 0.5f, 70.0f, kRate, s.buf.getNumSamples());
        check (markers.size() >= 7 && markers.size() <= 10, "default chop count is sane for sustained chords (" + juce::String (markers.size()) + ")");
        std::vector<float> st;
        for (const auto& o : a.onsets)
            st.push_back (o.strength);
        std::sort (st.begin(), st.end(), std::greater<float>());
        std::cout << "    strongest onsets:";
        for (size_t i = 0; i < juce::jmin ((size_t) 14, st.size()); ++i)
            std::cout << " " << juce::String (st[i], 2);
        std::cout << "\n";
    }
    if (withDrums)
    {
        check (bpmMatches (a.tempo.bpm, bpm), "tempo exactly " + juce::String (bpm));
        check (std::abs (a.grid.downbeatSeconds) < 0.02, "downbeat at 0");
    }

    // Per-slice harmony on each bar
    for (int b = 0; b < bars; ++b)
    {
        const auto start = (juce::int64) (b * 4 * beat * kRate);
        const auto d = describeSlice (a.features, s.buf, start, start + (juce::int64) (4 * beat * kRate));
        std::cout << "    bar " << b + 1 << ": type " << d.type << ", harmony '" << d.harmony << "', peak " << d.peakDb << " dB\n";
    }
}

void testWaltz()
{
    std::cout << "3/4 waltz, 120 BPM, 8 bars (C G Am F)\n";
    const double bpm = 120.0, beat = 0.5;
    const int bars = 8;
    Synth s (bars * 3 * beat);
    const std::vector<std::vector<int>> chords { { 48, 52, 55 }, { 43, 47, 50 }, { 45, 48, 52 }, { 41, 45, 48 } };
    for (int b = 0; b < bars; ++b)
    {
        const double t = b * 3 * beat;
        s.kick (t);
        s.hat (t + beat, 0.25f);
        s.hat (t + 2 * beat, 0.25f);
        for (int n : chords[(size_t) b % 4])
            s.note (t, 3 * beat, n + 12);
        s.note (t + beat, beat, chords[(size_t) b % 4][1] + 24, 0.06f);
        s.note (t + 2 * beat, beat, chords[(size_t) b % 4][2] + 24, 0.06f);
    }

    const auto a = analyse (s.buf, kRate);
    check (bpmMatches (a.tempo.bpm, bpm), "tempo 120");
    check (a.grid.timeSig == TimeSignature { 3, 4 }, "meter 3/4");
    check (std::abs (a.grid.downbeatSeconds) < 0.02, "downbeat at 0");
}

void testOffsetNonLoop()
{
    std::cout << "Untrimmed 96 BPM groove: 0.37 s pickup gap, starts on beat 3, 1.5 s tail\n";
    const double bpm = 96.0, beat = 60.0 / bpm, offset = 0.37;
    Synth s (offset + 6 * 4 * beat + 1.5);
    // first audible hit is beat 3 of a bar
    const double barZero = offset - 2 * beat;
    for (int b = 0; b < 6; ++b)
    {
        const double t = barZero + b * 4 * beat;
        if (b == 0)
        {
            s.kick (t + 2 * beat);
            s.snare (t + 3 * beat);
            s.hat (t + 2.5 * beat);
            s.hat (t + 3.5 * beat);
            continue;
        }
        drumBar (s, t, beat);
    }

    const auto a = analyse (s.buf, kRate);
    check (bpmMatches (a.tempo.bpm, bpm), "tempo 96");
    check (! a.tempo.loopDetected, "not flagged as a loop");
    const double expectedDownbeat = barZero + 4 * beat;
    check (std::abs (a.grid.downbeatSeconds - expectedDownbeat) < 0.02,
           "first bar start found at " + juce::String (expectedDownbeat, 3) + " s (got " + juce::String (a.grid.downbeatSeconds, 3) + ")");
}

void testGridPositions()
{
    std::cout << "Grid position labels\n";
    const TimeSignature ts { 4, 4 };
    const double bpm = 120.0; // beat = 0.5 s, 16th = 0.125 s
    check (gridPositionAt (0.0, bpm, 0.0, ts).describe == "Bar start", "0 s -> Bar start");
    auto p = gridPositionAt (2.0 + 0.5, bpm, 0.0, ts);
    check (p.bar == 2 && p.beat == 2 && p.describe == "Beat 2", "2.5 s -> bar 2 beat 2");
    check (gridPositionAt (0.75, bpm, 0.0, ts).describe == "2 &", "0.75 s -> 2 &");
    check (gridPositionAt (0.625, bpm, 0.0, ts).describe == "2 e", "0.625 s -> 2 e");
    check (gridPositionAt (0.06, bpm, 0.0, ts).describe == "Off grid", "0.06 s -> off grid");
    check (gridPositionAt (0.0, bpm, 1.0, ts).bar == 0, "before the first downbeat -> pickup (bar 0)");
    const auto w = gridPositionAt (1.5, bpm, 0.0, { 3, 4 });
    check (w.bar == 2 && w.describe == "Bar start", "3/4: 1.5 s -> bar 2 start");
    const auto e = gridPositionAt (0.25, bpm, 0.0, { 6, 8 });
    check (e.beat == 2 && e.describe == "Beat 2", "6/8: 0.25 s -> eighth-note beat 2");
    check (midiNoteName (60) == "C5", "MIDI 60 is C5 (FL naming)");
}

void analyseFile (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr)
    {
        std::cout << file.getFullPathName() << ": can't read\n";
        return;
    }
    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    std::cout << file.getFileName() << "\n";
    analyse (b, r->sampleRate);
}
} // namespace

int downloadModelCommand (int model);
int printLyricsCommand (const juce::File& file, const juce::String& lang, int model);
int lyricsTestCommand (const juce::File& fixtures, int model);
int lyricsSnapTest (const juce::File& fixtures);

int main (int argc, char** argv)
{
    const juce::String command = argc > 1 ? juce::String (argv[1]) : juce::String();
    auto arg = [&] (int i) { return i < argc ? juce::String (argv[i]) : juce::String(); };
    auto cwdFile = [] (const juce::String& p) { return juce::File::getCurrentWorkingDirectory().getChildFile (p); };
    if (command == "--download-model")
        return downloadModelCommand (arg (2).getIntValue());
    if (command == "--lyrics-test")
        return lyricsTestCommand (cwdFile (arg (2)), arg (3).getIntValue());
    if (command == "--lyrics")
        return printLyricsCommand (cwdFile (arg (2)), arg (3), arg (4).getIntValue());

    if (argc > 2 && juce::String (argv[1]) == "--write-demo")
    {
        // 4-bar A minor groove at 90 BPM, for trying the plugin out
        const double beat = 60.0 / 90.0;
        Synth s (16 * beat);
        const std::vector<std::vector<int>> chords { { 57, 60, 64, 45 }, { 53, 57, 60, 41 }, { 48, 52, 55, 60 }, { 55, 59, 62, 43 } };
        for (int b = 0; b < 4; ++b)
        {
            for (int n : chords[(size_t) b])
                s.note (b * 4 * beat, 4 * beat, n);
            s.note (b * 4 * beat + 2.5 * beat, 0.5 * beat, chords[(size_t) b][0] + 12, 0.1f);
            s.note (b * 4 * beat + 3.0 * beat, beat, chords[(size_t) b][1] + 12, 0.1f);
            drumBar (s, b * 4 * beat, beat);
        }
        s.buf.applyGain (0.8f / s.buf.getMagnitude (0, s.buf.getNumSamples()));
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        out.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (kRate).withNumChannels (2).withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (s.buf, 0, s.buf.getNumSamples());
        return 0;
    }

    if (argc > 3 && juce::String (argv[1]) == "--standalone-state")
    {
        // Points the standalone app at a sample on next launch (it restores "filterState" from its settings file)
        juce::ValueTree t ("ChopLab");
        t.setProperty ("file", juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]).getFullPathName(), nullptr);
        juce::MemoryOutputStream state;
        t.writeToStream (state);
        juce::PropertiesFile::Options o;
        juce::PropertiesFile props (juce::File (argv[3]), o);
        props.setValue ("filterState", state.getMemoryBlock().toBase64Encoding());
        return props.saveIfNeeded() ? 0 : 1;
    }

    if (argc > 1)
    {
        for (int i = 1; i < argc; ++i)
            analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]));
        return 0;
    }

    testGridPositions();
    for (double bpm : { 87.0, 93.0, 120.0, 140.0, 174.0 })
        testDrumLoop (bpm, 4);
    testDrumLoop (72.0, 2);
    testChordLoop (90.0, false);
    testChordLoop (90.0, true);
    testWaltz();
    testOffsetNonLoop();

    if (const auto fixtures = juce::File::getCurrentWorkingDirectory().getChildFile ("Tests/fixtures"); fixtures.isDirectory())
        failures += lyricsSnapTest (fixtures);

    std::cout << "\n" << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
