// Drives ChopLabProcessor directly (no plugin wrapper) to test the pattern player, undo, MIDI
// export, lyrics plumbing and state round-trips. Built from the same sources as the plugin.

#include "PluginProcessor.h"
#include "TestSignals.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>

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

struct FixedPlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        return p;
    }
};

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

juce::AudioBuffer<float> render (ChopLabProcessor& p, double seconds)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = juce::jmin (512, total - pos);
        juce::AudioBuffer<float> buf (2, n);
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            out.copyFrom (c, pos, buf, c, 0, n);
    }
    return out;
}

bool waitFor (std::function<bool()> condition, int timeoutMs = 20000)
{
    for (int t = 0; t < timeoutMs; t += 50)
    {
        if (condition())
            return true;
        pump (50);
    }
    return condition();
}

// Times where the output jumps from quiet to loud
std::vector<double> onsetTimes (const juce::AudioBuffer<float>& b)
{
    std::vector<double> times;
    // 2 ms windows; an onset is a loud window whose level 4 ms earlier was near silence
    const int block = (int) (kRate * 0.002);
    std::vector<float> levels;
    for (int i = 0; i + block <= b.getNumSamples(); i += block)
        levels.push_back (b.getRMSLevel (0, i, block));
    double lastOnset = -1.0;
    for (size_t k = 0; k < levels.size(); ++k)
    {
        const float before = k >= 2 ? levels[k - 2] : 0.0f;
        const double t = (double) (k * (size_t) block) / kRate;
        if (levels[k] > 0.02f && before < 0.004f && t - lastOnset > 0.05)
        {
            // step back to the window where the sound begins
            const size_t first = k >= 1 && levels[k - 1] > 0.001f ? k - 1 : k;
            times.push_back ((double) (first * (size_t) block) / kRate);
            lastOnset = t;
        }
    }
    return times;
}

juce::File writeLoop (double bpm)
{
    const double beat = 60.0 / bpm;
    Synth loop (16 * beat);
    for (int b = 0; b < 4; ++b)
        drumBar (loop, b * 4 * beat, beat);
    const auto file = juce::File::createTempFile (".wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (kRate).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (loop.buf, 0, loop.buf.getNumSamples());
    return file;
}
} // namespace

// Builds a ready-made project for screenshots: a sample, a few labels, lyrics words from a text
// file ("seconds word" per line) and the piano roll filled from the sample's order.
static int writeDemoState (const juce::File& audio, const juce::File& wordsFile, const juce::File& settingsOut, bool withStems)
{
    ChopLabProcessor p;
    p.prepareToPlay (kRate, 512);
    p.loadFile (audio);
    if (! waitFor ([&] { return ! p.isAnalysing() && p.doc().hasSample(); }))
        return 1;
    if (withStems)
    {
        // Stand-in stems (the mix at different levels), for looking at the layout without the model.
        StemsResult fake;
        for (int i = 0; i < kNumStems; ++i)
        {
            fake.stems[(size_t) i].makeCopyOf (p.doc().sample->audio);
            fake.stems[(size_t) i].applyGain (0.3f + 0.15f * (float) i);
        }
        p.useSeparatedStems (fake);
        p.setSource (stemVocals);
        if (! waitFor ([&] { return ! p.isAnalysing() && p.doc().source == stemVocals; }))
            return 1;
    }
    p.fillPatternFromSample();
    auto s0 = p.doc().slices[0].settings;
    s0.label = "kick";
    p.setSliceSettings (0, s0);

    juce::MemoryBlock state;
    p.getStateInformation (state);
    auto tree = juce::ValueTree::readFromData (state.getData(), state.getSize());
    auto lyrics = tree.getChildWithName ("Lyrics");
    lyrics.setProperty ("language", "en", nullptr);
    lyrics.setProperty ("searched", true, nullptr);
    juce::StringArray lines;
    lines.addLines (wordsFile.loadFileAsString());
    for (int i = 0; i < lines.size(); ++i)
    {
        if (lines[i].trim().isEmpty())
            continue;
        const double t = lines[i].upToFirstOccurrenceOf (" ", false, false).getDoubleValue();
        const double next = i + 1 < lines.size() && lines[i + 1].trim().isNotEmpty() ? lines[i + 1].getDoubleValue() : t + 0.4;
        juce::ValueTree w ("W");
        w.setProperty ("t", lines[i].fromFirstOccurrenceOf (" ", false, false).trim(), nullptr);
        w.setProperty ("s", t, nullptr);
        w.setProperty ("e", juce::jmin (next, t + 0.5), nullptr);
        lyrics.appendChild (w, nullptr);
    }
    juce::MemoryOutputStream out;
    tree.writeToStream (out);
    juce::PropertiesFile::Options o;
    juce::PropertiesFile props (settingsOut, o);
    props.setValue ("filterState", out.getMemoryBlock().toBase64Encoding());
    return props.saveIfNeeded() ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 4 && juce::String (argv[1]) == "--demo-state")
    {
        auto f = [] (const char* path) { return juce::File::getCurrentWorkingDirectory().getChildFile (path); };
        return writeDemoState (f (argv[2]), f (argv[3]), f (argv[4]), argc > 5 && juce::String (argv[5]) == "stems");
    }
    const auto fixtures = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "Tests/fixtures");

    FixedPlayHead playHead;
    auto proc = std::make_unique<ChopLabProcessor>();
    proc->setPlayConfigDetails (0, 2, kRate, 512);
    proc->prepareToPlay (kRate, 512);
    proc->setPlayHead (&playHead);

    std::cout << "Load a 93 BPM drum loop\n";
    const auto loopFile = writeLoop (93.0);
    proc->loadFile (loopFile);
    check (waitFor ([&] { return ! proc->isAnalysing() && proc->doc().slices.size() == 32; }), "loaded and chopped into 32");
    pump (500);
    render (*proc, 0.05);

    std::cout << "Pattern playback at 120 BPM (0.5 s per beat)\n";
    Pattern p;
    p.lengthBeats = 4.0;
    p.notes = { { 0, 0.0, 0.5, 1.0f }, { 4, 1.0, 0.5, 1.0f }, { 0, 2.5, 0.5, 1.0f } };
    proc->setPattern (p);
    proc->playPattern (true);
    auto out = render (*proc, 2.2);
    proc->playPattern (false);
    const auto onsets = onsetTimes (out);
    std::cout << "  onsets:";
    for (auto t : onsets)
        std::cout << " " << juce::String (t, 3);
    std::cout << "\n";
    const std::vector<double> expected { 0.0, 0.5, 1.25, 2.0 };
    bool timingOk = onsets.size() >= expected.size();
    for (size_t i = 0; timingOk && i < expected.size(); ++i)
        timingOk = std::abs (onsets[i] - expected[i]) < 0.006;
    check (timingOk, "notes play at 0, 0.5, 1.25 s and loop back at 2.0 s");
    check (out.getRMSLevel (0, (int) (0.30 * kRate), (int) (0.15 * kRate)) < 1.0e-3, "notes stop at their length (gate)");
    out = render (*proc, 0.3);
    check (out.getRMSLevel (0, (int) (0.05 * kRate), (int) (0.2 * kRate)) < 1.0e-4, "stopping the pattern silences it");

    std::cout << "A note can start partway into its chop\n";
    {
        Pattern late;
        late.lengthBeats = 4.0;
        late.notes = { { 0, 0.0, 1.0, 1.0f, 0.1 } }; // chop 1, starting 0.1 s in
        proc->setPattern (late);
        proc->playPattern (true);
        const auto played = render (*proc, 0.3);
        proc->playPattern (false);
        render (*proc, 0.1);
        const auto& src = proc->doc().sample->audio;
        const int from = (int) proc->doc().slices[0].start + (int) (0.1 * kRate);
        const int chopEnd = (int) proc->doc().slices[0].end;
        float worst = 0.0f, loudest = 0.0f;
        for (int i = (int) (0.003 * kRate); i < juce::jmin ((int) (0.25 * kRate), chopEnd - from - (int) (0.004 * kRate)); ++i)
        {
            worst = juce::jmax (worst, std::abs (played.getSample (0, i) - src.getSample (0, from + i)));
            loudest = juce::jmax (loudest, std::abs (src.getSample (0, from + i)));
        }
        check (loudest > 0.01f && worst < 1.0e-4f, "plays the chop from 0.1 s in (largest difference " + juce::String (worst, 6) + ")");
    }
    proc->setPattern (p);

    std::cout << "Undo and chop edits keep the pattern pointing at the same audio\n";
    auto p2 = p;
    p2.notes.push_back ({ 8, 3.0, 0.5, 0.5f });
    proc->setPattern (p2);
    check (proc->doc().pattern.notes.size() == 4, "note added");
    proc->undo();
    check (proc->doc().pattern.notes.size() == 3, "undo removes it");
    proc->redo();
    check (proc->doc().pattern.notes.size() == 4, "redo brings it back");

    const auto& s = proc->doc().slices;
    const auto splitAt = (s[1].start + s[1].end) / 2;
    proc->addMarker (splitAt);
    check (proc->doc().slices.size() == 33, "chop 2 split in two");
    check (proc->doc().pattern.notes[1].chop == 5 && proc->doc().pattern.notes[3].chop == 9, "notes after the split moved up one chop");
    proc->removeMarker (2);
    check (proc->doc().pattern.notes[1].chop == 4 && proc->doc().pattern.notes[3].chop == 8, "and back down after merging");

    std::cout << "Pattern from the sample's own order\n";
    proc->fillPatternFromSample();
    const auto& filled = proc->doc().pattern;
    check (filled.notes.size() == 32, "one note per chop");
    check (std::abs (filled.lengthBeats - 16.0) < 1.0e-6, "4 bars long");
    check (std::abs (filled.notes[0].start) < 1.0e-6 && std::abs (filled.notes[4].start - 2.0) < 0.02, "notes sit where the chops were");

    std::cout << "MIDI export\n";
    const auto midiFile = proc->writePatternMidi (juce::File::getSpecialLocation (juce::File::tempDirectory));
    juce::MidiFile midi;
    juce::FileInputStream midiIn (midiFile);
    check (midiIn.openedOk() && midi.readFrom (midiIn), "MIDI file written and readable");
    int noteOns = 0;
    double firstOnBeat = -1.0;
    int firstNote = -1;
    if (midi.getNumTracks() > 0)
        for (const auto* e : *midi.getTrack (0))
            if (e->message.isNoteOn())
            {
                if (noteOns++ == 0)
                {
                    firstOnBeat = e->message.getTimeStamp() / midi.getTimeFormat();
                    firstNote = e->message.getNoteNumber();
                }
            }
    check (noteOns == 32 && firstNote == 60 && std::abs (firstOnBeat) < 1.0e-6, "32 notes, first is C5 on beat 1");

    std::cout << "Lyrics edits and state round-trip\n";
    proc->setSliceLyrics (3, "oh yeah");
    check (proc->doc().lyricsFor (3) == "oh yeah", "edited lyrics show on the chop");
    {
        auto withOffset = proc->doc().pattern;
        withOffset.notes[0].offset = 0.05;
        proc->setPattern (withOffset);
    }
    proc->setLyricsOptions (1, "es");
    juce::MemoryBlock state;
    proc->getStateInformation (state);

    auto reopened = std::make_unique<ChopLabProcessor>();
    reopened->prepareToPlay (kRate, 512);
    reopened->setStateInformation (state.getData(), (int) state.getSize());
    check (waitFor ([&] { return ! reopened->isAnalysing() && reopened->doc().slices.size() == 32; }), "project reopens");
    check (reopened->doc().pattern.notes.size() == 32 && std::abs (reopened->doc().pattern.lengthBeats - 16.0) < 1.0e-6, "pattern restored");
    check (reopened->doc().lyricsFor (3) == "oh yeah", "lyrics edit restored");
    check (std::abs (reopened->doc().pattern.notes[0].offset - 0.05) < 1.0e-9, "note start offset restored");
    check (reopened->doc().lyrics.model == 1 && reopened->doc().lyrics.requestedLanguage == "es", "lyrics settings restored");
    reopened = nullptr;

    std::cout << "Stems (made up here, so no model is needed)\n";
    {
        const auto mixPeak = proc->doc().sample->audio.getMagnitude (0, proc->doc().sample->audio.getNumSamples());
        auto peakPlaying = [] (const ChopLabProcessor& pr) { return pr.doc().sample->audio.getMagnitude (0, pr.doc().sample->audio.getNumSamples()); };
        StemsResult fake;
        for (int i = 0; i < kNumStems; ++i)
        {
            fake.stems[(size_t) i].makeCopyOf (proc->doc().sample->audio);
            fake.stems[(size_t) i].applyGain (0.1f * (float) (i + 1)); // each stem is the loop at its own level
        }
        std::vector<juce::int64> startsBefore;
        for (const auto& sl : proc->doc().slices)
            startsBefore.push_back (sl.start);
        const double bpmBefore = proc->doc().bpm;

        proc->useSeparatedStems (fake);
        check (proc->doc().stems != nullptr && proc->doc().source == -1, "stems kept, the mix still plays");
        bool filesOk = true;
        for (int i = 0; i < kNumStems; ++i)
            filesOk = filesOk && proc->stemFile (i).existsAsFile();
        check (filesOk, "stem files saved for dragging");

        proc->setSource (stemDrums);
        check (waitFor ([&] { return ! proc->isAnalysing() && proc->doc().source == stemDrums; }), "switched to the drum stem");
        check (std::abs (peakPlaying (*proc) - 0.2f * mixPeak) < 1.0e-4f, "chops play the drum stem");
        std::vector<juce::int64> startsAfter;
        for (const auto& sl : proc->doc().slices)
            startsAfter.push_back (sl.start);
        check (startsAfter == startsBefore && std::abs (proc->doc().bpm - bpmBefore) < 1.0e-9, "same chops and tempo");
        check (proc->doc().lyricsFor (3) == "oh yeah" && proc->doc().pattern.notes.size() == 32, "lyrics and pattern untouched");
        check (proc->canUndo(), "undo history kept");
        pump (300);
        proc->previewSlice (0);
        const auto preview = render (*proc, 0.3);
        const float chopPeak = proc->doc().sample->audio.getMagnitude (0, (int) proc->doc().slices[0].end);
        check (std::abs (preview.getMagnitude (0, preview.getNumSamples()) - chopPeak) < 0.02f, "and what plays is the stem");

        // One chop can play a different stem from the rest
        auto bassChop = proc->doc().slices[3].settings;
        bassChop.stem = stemBass;
        proc->setSliceSettings (3, bassChop);
        pump (300);
        proc->previewSlice (3);
        const auto chop4 = render (*proc, 0.25);
        const auto& s3 = proc->doc().slices[3];
        const auto& mixAudio = proc->doc().mix()->audio;
        const int span = juce::jmin ((int) (s3.end - s3.start), (int) (0.25 * kRate));
        const float mixPeak3 = mixAudio.getMagnitude (0, (int) s3.start, span);
        check (std::abs (chop4.getMagnitude (0, chop4.getNumSamples()) - 0.3f * mixPeak3) < 0.02f,
               "chop 4 plays the bass stem while the others play the drums");

        juce::MemoryBlock stemState;
        proc->getStateInformation (stemState);
        auto reopened = std::make_unique<ChopLabProcessor>();
        reopened->prepareToPlay (kRate, 512);
        reopened->setStateInformation (stemState.getData(), (int) stemState.getSize());
        check (waitFor ([&] { return ! reopened->isAnalysing() && reopened->doc().hasSample(); }), "project with stems reopens");
        check (reopened->doc().stems != nullptr && reopened->doc().source == stemDrums
                   && std::abs (peakPlaying (*reopened) - 0.2f * mixPeak) < 1.0e-4f,
               "still playing the drum stem, read back from its file");
        check (std::abs (reopened->doc().mix()->audio.getMagnitude (0, reopened->doc().mix()->audio.getNumSamples()) - mixPeak) < 1.0e-3f,
               "the project itself holds the full mix");
        check (reopened->doc().slices[3].settings.stem == stemBass, "chop 4 still plays the bass stem");
        reopened = nullptr;

        const auto lastDrum = proc->doc().sample->audio.getSample (0, proc->doc().sample->audio.getNumSamples() - 1);
        proc->setReversed (true);
        check (waitFor ([&] { return ! proc->isAnalysing() && proc->doc().reversed; }), "reversed");
        check (proc->doc().stems != nullptr && proc->doc().source == stemDrums
                   && std::abs (proc->doc().sample->audio.getSample (0, 0) - lastDrum) < 1.0e-6f,
               "the stems reverse with it");
        proc->setReversed (false);
        check (waitFor ([&] { return ! proc->isAnalysing() && ! proc->doc().reversed; }), "and back");

        for (const auto& f : proc->doc().stems->files)
            f.deleteFile();
        auto again = std::make_unique<ChopLabProcessor>();
        again->prepareToPlay (kRate, 512);
        again->setStateInformation (stemState.getData(), (int) stemState.getSize());
        check (waitFor ([&] { return ! again->isAnalysing() && again->doc().hasSample(); }), "reopens with the stem files gone");
        check (again->doc().stems == nullptr && again->doc().source == -1 && again->getError().isNotEmpty(),
               "falls back to the mix and says why: " + again->getError());
        again = nullptr;
        check (proc->stemFile (stemVocals).existsAsFile(), "a deleted stem file is written again when it's dragged");

        proc->setSource (-1);
        check (waitFor ([&] { return ! proc->isAnalysing() && proc->doc().source == -1; }) && std::abs (peakPlaying (*proc) - mixPeak) < 1.0e-4f,
               "back to the full mix");
        proc->getDragFolder().deleteRecursively();
    }

    std::cout << "Finding lyrics through the plugin\n";
    const auto speech = fixtures.getChildFile ("speech_en.wav");
    proc->loadFile (speech);
    check (waitFor ([&] { return ! proc->isAnalysing() && proc->doc().hasSample(); }), "speech sample loaded");
    proc->setLyricsOptions (0, {});
    const bool haveModel = lyricsModelFile (0).existsAsFile();
    proc->findLyrics();
    const bool finished = waitFor ([&]
    {
        const auto st = proc->getLyricsStatus().phase;
        return st == ChopLabProcessor::LyricsStatus::done || st == ChopLabProcessor::LyricsStatus::failed;
    }, 240000);
    const auto status = proc->getLyricsStatus();
    if (haveModel)
    {
        check (finished && status.phase == ChopLabProcessor::LyricsStatus::done, "lyrics found");
        check (proc->doc().lyrics.language == "en" && proc->doc().lyrics.words.size() >= 4,
               "words stored (" + juce::String (proc->doc().lyrics.words.size()) + ", " + proc->doc().lyrics.language + ")");
        juce::String all;
        for (int i = 0; i < (int) proc->doc().slices.size(); ++i)
            all << proc->doc().lyricsFor (i) << " ";
        check (all.toLowerCase().contains ("hello"), "chops show their words: " + all.trim());

        // With stems, the words come from the vocal stem: make it Spanish speech and see.
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> es (formats.createReaderFor (fixtures.getChildFile ("speech_es.wav")));
        const auto& mixAudio = proc->doc().sample->audio;
        StemsResult fake;
        for (auto& stem : fake.stems)
        {
            stem.setSize (mixAudio.getNumChannels(), mixAudio.getNumSamples());
            stem.clear();
        }
        es->read (&fake.stems[stemVocals], 0, juce::jmin ((int) es->lengthInSamples, mixAudio.getNumSamples()), 0, true, false);
        proc->useSeparatedStems (fake);
        proc->findLyrics();
        waitFor ([&]
        {
            const auto st = proc->getLyricsStatus().phase;
            return st == ChopLabProcessor::LyricsStatus::done || st == ChopLabProcessor::LyricsStatus::failed;
        }, 240000);
        check (proc->doc().lyrics.language == "es", "with stems, lyrics come from the vocal stem (heard " + proc->doc().lyrics.language + ")");
        proc->getDragFolder().deleteRecursively();
    }
    else
    {
        std::cout << "  (no model on this machine: checking the failure path)\n";
        check (finished && status.phase == ChopLabProcessor::LyricsStatus::failed && status.message.isNotEmpty(),
               "a clear error when the model can't be downloaded: " + status.message);
    }

    proc = nullptr;
    loopFile.deleteFile();
    std::cout << "\n" << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
