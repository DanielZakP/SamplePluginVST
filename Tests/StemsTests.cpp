// Stem separation tests. They need the real model, so they run separately (CI downloads it):
//   ChopLabTests --download-stems-model
//   ChopLabTests --stems-test Tests/fixtures [quick]
//   ChopLabTests --stems-bench seconds workers model.bin   (timing and memory, any model file)

#include "Engine/Renderer.h"
#include "Stems/Stems.h"
#include "TestSignals.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define PSAPI_VERSION 2
 #include <windows.h>
 #include <psapi.h>
#else
 #include <sys/resource.h>
#endif

using namespace choplab;

namespace
{
int stemFailures = 0;

void expect (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (! ok)
        ++stemFailures;
}

int peakMemoryMB()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS counters {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &counters, sizeof (counters)))
        return (int) (counters.PeakWorkingSetSize / (1024 * 1024));
    return 0;
   #else
    rusage usage {};
    getrusage (RUSAGE_SELF, &usage);
    return (int) (usage.ru_maxrss / 1024);
   #endif
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

float rms (const juce::AudioBuffer<float>& b)
{
    double sum = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            sum += (double) b.getSample (c, i) * b.getSample (c, i);
    return (float) std::sqrt (sum / juce::jmax (1, b.getNumChannels() * b.getNumSamples()));
}

// Scale-invariant signal-to-distortion ratio of estimate against reference, on the mono sums.
double siSdr (const juce::AudioBuffer<float>& estimate, const juce::AudioBuffer<float>& reference)
{
    auto mono = [] (const juce::AudioBuffer<float>& b, int i)
    {
        double v = 0.0;
        for (int c = 0; c < b.getNumChannels(); ++c)
            v += b.getSample (c, i);
        return v / b.getNumChannels();
    };
    const int n = juce::jmin (estimate.getNumSamples(), reference.getNumSamples());
    double dot = 0.0, refEnergy = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double r = mono (reference, i);
        dot += mono (estimate, i) * r;
        refEnergy += r * r;
    }
    const double alpha = dot / juce::jmax (1.0e-12, refEnergy);
    double target = 0.0, noise = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = alpha * mono (reference, i);
        const double e = mono (estimate, i) - t;
        target += t * t;
        noise += e * e;
    }
    return 10.0 * std::log10 (juce::jmax (1.0e-12, target) / juce::jmax (1.0e-12, noise));
}

struct TestMix
{
    juce::AudioBuffer<float> mix, vocals, drums, bass;
};

// Speech for the vocal, synthetic drums and a sine bass line, at about the same loudness.
TestMix makeMix (const juce::File& fixtures, double seconds)
{
    TestMix m;
    const int n = (int) (seconds * kRate);

    juce::AudioBuffer<float> speech;
    double speechRate = 0.0;
    readWav (fixtures.getChildFile ("speech_en.wav"), speech, speechRate);
    speech = resampleAudio (speech, speechRate / kRate);
    m.vocals.setSize (2, n);
    m.vocals.clear();
    const int start = (int) (0.25 * kRate);
    for (int c = 0; c < 2; ++c)
        m.vocals.copyFrom (c, start, speech, 0, 0, juce::jmin (speech.getNumSamples(), n - start));

    const double beat = 60.0 / 100.0;
    Synth d (seconds);
    for (double bar = 0.0; bar < seconds; bar += 4.0 * beat)
        drumBar (d, bar, beat);
    m.drums.makeCopyOf (d.buf);
    m.drums.setSize (2, n, true);

    m.bass.setSize (2, n);
    m.bass.clear();
    const double notes[] { 55.0, 55.0, 65.41, 73.42, 55.0, 49.0, 55.0, 82.41 };
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kRate;
        const int eighth = (int) (t / (beat * 0.5));
        const double inNote = t - eighth * beat * 0.5;
        phase += 2.0 * kPi * notes[eighth % 8] / kRate;
        const double env = std::min (1.0, inNote * 300.0) * std::exp (-inNote * 3.0) * std::min (1.0, (beat * 0.5 - inNote) * 200.0);
        const float v = (float) ((std::sin (phase) + 0.2 * std::sin (2.0 * phase)) * env);
        m.bass.setSample (0, i, v);
        m.bass.setSample (1, i, v);
    }

    for (auto* part : { &m.vocals, &m.drums, &m.bass })
        part->applyGain (0.12f / juce::jmax (1.0e-6f, rms (*part)));
    m.mix.makeCopyOf (m.vocals);
    for (int c = 0; c < 2; ++c)
    {
        m.mix.addFrom (c, 0, m.drums, c, 0, n);
        m.mix.addFrom (c, 0, m.bass, c, 0, n);
    }
    return m;
}

StemsResult timedSeparation (const juce::AudioBuffer<float>& audio, double rate, int workers, double& seconds)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = separateStems (audio, rate, [] (float) { return true; }, workers);
    seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    return r;
}

float maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float worst = 0.0f;
    for (int c = 0; c < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++c)
        for (int i = 0; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
            worst = juce::jmax (worst, std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return worst;
}
} // namespace

int downloadStemsModelCommand()
{
    std::cout << "Downloading the stem model to " << stemsModelFile().getFullPathName() << "\n";
    juce::String error;
    int lastPercent = -10;
    const bool ok = downloadStemsModel ([&] (float f)
    {
        const int percent = (int) (f * 100.0f);
        if (percent >= lastPercent + 10)
        {
            std::cout << "  " << percent << "%\n";
            lastPercent = percent;
        }
        return true;
    }, error);
    std::cout << (ok ? "done, " + juce::String (stemsModelFile().getSize()) + " bytes" : "FAILED: " + error) << "\n";
    return ok ? 0 : 1;
}

int stemsBenchCommand (double seconds, int workers, const juce::File& model)
{
    Synth s (seconds);
    for (double bar = 0.0; bar < seconds; bar += 2.4)
        drumBar (s, bar, 0.6);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto r = separateStems (s.buf, kRate, [] (float) { return true; }, workers, model);
    const double took = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::cout << seconds << " s of audio, " << workers << " threads: " << juce::String (took, 1) << " s, peak memory " << peakMemoryMB()
              << " MB" << (r.error.isNotEmpty() ? ", error: " + r.error : juce::String()) << "\n";
    return r.error.isEmpty() ? 0 : 1;
}

int stemsTestCommand (const juce::File& fixtures, bool quick)
{
    std::cout << "Stem separation\n";
    expect (stemsSupportedOnThisCpu(), "this CPU can run it");
    if (! stemsModelLooksComplete())
    {
        expect (false, "model is downloaded (" + stemsModelFile().getFullPathName() + ")");
        return 1;
    }
    std::cout << "  model " << stemsModelFile().getSize() << " bytes, " << stemWorkerCount() << " threads on this machine ("
              << juce::SystemStats::getNumPhysicalCpus() << " cores, " << juce::SystemStats::getMemorySizeInMegabytes() << " MB)\n";

    const double seconds = 8.0;
    const auto m = makeMix (fixtures, seconds);

    double took = 0.0;
    const auto a = timedSeparation (m.mix, kRate, 0, took);
    expect (a.error.isEmpty() && ! a.cancelled, "separates" + (a.error.isNotEmpty() ? ": " + a.error : juce::String()));
    if (a.error.isNotEmpty())
        return 1;
    std::cout << "  " << seconds << " s of audio in " << juce::String (took, 1) << " s (" << juce::String (took / seconds, 2)
              << " s per second of audio), peak memory " << peakMemoryMB() << " MB\n";

    bool shapesOk = true;
    for (const auto& s : a.stems)
        shapesOk = shapesOk && s.getNumChannels() == 2 && s.getNumSamples() == m.mix.getNumSamples();
    expect (shapesOk, "every stem is stereo and as long as the input");

    const double vocals = siSdr (a.stems[stemVocals], m.vocals);
    const double drums = siSdr (a.stems[stemDrums], m.drums);
    const double bass = siSdr (a.stems[stemBass], m.bass);
    juce::AudioBuffer<float> backing (m.drums);
    for (int c = 0; c < 2; ++c)
        backing.addFrom (c, 0, m.bass, c, 0, backing.getNumSamples());
    const double instrumental = siSdr (a.stems[stemInstrumental], backing);
    const double mixAsVocals = siSdr (m.mix, m.vocals);
    std::cout << "  SI-SDR (dB): vocals " << juce::String (vocals, 1) << ", drums " << juce::String (drums, 1) << ", bass "
              << juce::String (bass, 1) << ", instrumental " << juce::String (instrumental, 1) << " (the unseparated mix scores "
              << juce::String (mixAsVocals, 1) << " as vocals)\n";
    expect (vocals > 6.0, "vocal stem is the voice (" + juce::String (vocals, 1) + " dB)");
    expect (drums > 6.0, "drum stem is the drums (" + juce::String (drums, 1) + " dB)");
    expect (bass > 3.0, "bass stem is the bass (" + juce::String (bass, 1) + " dB)");
    expect (instrumental > 6.0, "instrumental is drums + bass (" + juce::String (instrumental, 1) + " dB)");
    expect (rms (a.stems[stemOther]) < 0.5f * rms (m.mix), "little is left over in Other");

    if (! quick)
    {
        double tookSingle = 0.0;
        const auto b = timedSeparation (m.mix, kRate, 1, tookSingle);
        float worst = 0.0f;
        for (int s = 0; s < kNumStems; ++s)
            worst = juce::jmax (worst, maxDifference (a.stems[(size_t) s], b.stems[(size_t) s]));
        std::cout << "  one thread: " << juce::String (tookSingle, 1) << " s, largest difference " << worst << "\n";
        expect (b.error.isEmpty() && worst < 1.0e-3f, "one thread gives the same stems as several");
    }

    // Other formats: 48 kHz mono comes back as 48 kHz mono, still separated.
    {
        juce::AudioBuffer<float> mono (1, m.mix.getNumSamples());
        mono.copyFrom (0, 0, m.mix, 0, 0, mono.getNumSamples());
        auto mono48 = resampleAudio (mono, kRate / 48000.0);
        auto vocals48 = resampleAudio (m.vocals, kRate / 48000.0);
        const int fiveSeconds = 5 * 48000;
        mono48.setSize (1, fiveSeconds, true);
        vocals48.setSize (2, fiveSeconds, true);
        double t = 0.0;
        const auto r = timedSeparation (mono48, 48000.0, 0, t);
        const bool shape = r.error.isEmpty() && r.stems[stemVocals].getNumChannels() == 1 && r.stems[stemVocals].getNumSamples() == fiveSeconds;
        expect (shape, "48 kHz mono in, 48 kHz mono stems out");
        if (shape)
        {
            const double v = siSdr (r.stems[stemVocals], vocals48);
            expect (v > 4.0, "still separates at 48 kHz (" + juce::String (v, 1) + " dB vocals)");
        }
    }

    // Cancel stops it within a few seconds.
    {
        bool cancelled = false;
        double cancelledAt = juce::Time::getMillisecondCounterHiRes();
        const auto r = separateStems (m.mix, kRate, [&] (float f)
        {
            if (f > 0.02f && ! cancelled)
            {
                cancelled = true;
                cancelledAt = juce::Time::getMillisecondCounterHiRes();
            }
            return ! cancelled;
        });
        const double stopped = (juce::Time::getMillisecondCounterHiRes() - cancelledAt) / 1000.0;
        expect (r.cancelled && r.error.isEmpty(), "cancel reports cancelled");
        expect (stopped < 15.0, "cancel stops within 15 s (took " + juce::String (stopped, 1) + " s)");
    }

    // A damaged or missing model is reported, not loaded.
    {
        const auto damaged = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("choplab-damaged-model.bin");
        {
            juce::FileInputStream in (stemsModelFile());
            damaged.deleteFile();
            juce::FileOutputStream out (damaged);
            out.writeFromInputStream (in, 1 << 20);
        }
        const auto r = separateStems (m.mix, kRate, [] (float) { return true; }, 0, damaged);
        expect (r.error.contains ("damaged"), "a cut-off model file is caught: " + r.error);
        damaged.deleteFile();
        const auto missing = separateStems (m.mix, kRate, [] (float) { return true; }, 0, damaged);
        expect (missing.error.isNotEmpty(), "a missing model file is caught: " + missing.error);
    }

    std::cout << (stemFailures == 0 ? "ALL PASSED" : juce::String (stemFailures) + " FAILED") << "\n";
    return stemFailures == 0 ? 0 : 1;
}
