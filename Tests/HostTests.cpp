// Loads the built ChopLab VST3 the way a DAW does, feeds it a drum loop through its saved
// state, plays MIDI into it and checks the audio that comes out.
//   ChopLabHostTests path/to/ChopLab.vst3

#include "TestSignals.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>

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
        p.setIsPlaying (false);
        return p;
    }
};

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

// JUCE's VST3 host wraps the plugin's own state in XML (base64 under <IComponent>).
juce::ValueTree readState (juce::AudioPluginInstance& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    if (xml == nullptr || xml->getChildByName ("IComponent") == nullptr)
        return {};
    juce::MemoryBlock raw;
    raw.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText());
    return juce::ValueTree::readFromData (raw.getData(), raw.getSize());
}

void writeState (juce::AudioPluginInstance& p, const juce::ValueTree& t)
{
    juce::MemoryOutputStream out;
    t.writeToStream (out);
    juce::XmlElement xml ("VST3PluginState");
    xml.createNewChildElement ("IComponent")->addTextElement (out.getMemoryBlock().toBase64Encoding());
    juce::MemoryBlock wrapped;
    juce::AudioProcessor::copyXmlToBinary (xml, wrapped);
    p.setStateInformation (wrapped.getData(), (int) wrapped.getSize());
}

struct Event
{
    int sample;
    juce::MidiMessage message;
};

juce::AudioBuffer<float> render (juce::AudioPluginInstance& p, double seconds, const std::vector<Event>& events)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = juce::jmin (512, total - pos);
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        juce::MidiBuffer midi;
        for (const auto& e : events)
            if (e.sample >= pos && e.sample < pos + n)
                midi.addEvent (e.message, e.sample - pos);
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            out.copyFrom (c, pos, buf, c, 0, n);
    }
    return out;
}

double rms (const juce::AudioBuffer<float>& b, int start, int len)
{
    start = juce::jlimit (0, b.getNumSamples(), start);
    len = juce::jlimit (0, b.getNumSamples() - start, len);
    return len > 0 ? b.getRMSLevel (0, start, len) : 0.0;
}

// Seconds from `from` until 95% of the sound's energy has played (robust to stretch smearing)
double audibleSeconds (const juce::AudioBuffer<float>& b, int from)
{
    double total = 0.0;
    for (int i = from; i < b.getNumSamples(); ++i)
        total += (double) b.getSample (0, i) * b.getSample (0, i);
    double acc = 0.0;
    for (int i = from; i < b.getNumSamples(); ++i)
    {
        acc += (double) b.getSample (0, i) * b.getSample (0, i);
        if (acc >= total * 0.95)
            return (i - from) / kRate;
    }
    return 0.0;
}

// Waits until the plugin has finished analysing and its chops are rendered
bool waitForChops (juce::AudioPluginInstance& p, int minChops)
{
    for (int i = 0; i < 300; ++i)
    {
        pump (50);
        const auto st = readState (p);
        if (st.getChildWithName ("Slices").getNumChildren() >= minChops)
        {
            pump (400);
            render (p, 0.05, {}); // lets the audio thread pick up the rendered chops
            return true;
        }
    }
    return false;
}

juce::ValueTree sliceState (const juce::ValueTree& st, int index) { return st.getChildWithName ("Slices").getChild (index); }

std::unique_ptr<juce::AudioPluginInstance> loadPlugin (const juce::String& path, FixedPlayHead& playHead)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    if (types.isEmpty())
        return nullptr;
    juce::String error;
    auto instance = format.createInstanceFromDescription (*types[0], kRate, 512, error);
    if (instance == nullptr)
    {
        std::cout << "  load error: " << error << "\n";
        return nullptr;
    }
    instance->setPlayConfigDetails (0, 2, kRate, 512);
    instance->prepareToPlay (kRate, 512);
    instance->setPlayHead (&playHead);
    return instance;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2)
    {
        std::cout << "usage: ChopLabHostTests path/to/ChopLab.vst3\n";
        return 2;
    }
    const juce::String pluginPath = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]).getFullPathName();

    // A 4-bar, 93 BPM drum loop on disk
    const double bpm = 93.0, beat = 60.0 / bpm;
    Synth loop (16 * beat);
    for (int b = 0; b < 4; ++b)
        drumBar (loop, b * 4 * beat, beat);
    const auto wavFile = juce::File::createTempFile (".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (wavFile);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (kRate).withNumChannels (2).withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (loop.buf, 0, loop.buf.getNumSamples());
    }

    FixedPlayHead playHead;
    std::cout << "Load the VST3 and hand it a sample\n";
    auto plugin = loadPlugin (pluginPath, playHead);
    check (plugin != nullptr, "plugin loads as a VST3 instrument");
    if (plugin == nullptr)
        return 1;
    check (plugin->acceptsMidi() && plugin->getTotalNumOutputChannels() == 2, "takes MIDI, outputs stereo");

    juce::ValueTree init0 ("ChopLab");
    init0.setProperty ("file", wavFile.getFullPathName(), nullptr);
    writeState (*plugin, init0);
    check (waitForChops (*plugin, 2), "sample analysed and chopped");

    auto st = readState (*plugin);
    const int chops = st.getChildWithName ("Slices").getNumChildren();
    std::cout << "  bpm " << (double) st["bpm"] << ", " << (int) st["tsNum"] << "/" << (int) st["tsDen"] << ", " << chops << " chops, "
              << "embedded audio " << (st["audio"].getBinaryData() != nullptr ? (int) st["audio"].getBinaryData()->getSize() : 0) << " bytes\n";
    check (std::abs ((double) st["bpm"] - bpm) < 0.01, "tempo 93 BPM");
    check ((int) st["tsNum"] == 4 && (int) st["tsDen"] == 4, "4/4");
    check (chops == 32, "32 chops (one per hit)");
    check (st["audio"].getBinaryData() != nullptr && st["audio"].getBinaryData()->getSize() > 10000, "audio is embedded in the project state");

    std::cout << "MIDI playback\n";
    const int noteAt = (int) (0.1 * kRate);
    auto out = render (*plugin, 0.6, { { noteAt, juce::MidiMessage::noteOn (1, 60, 1.0f) }, { noteAt + (int) (0.25 * kRate), juce::MidiMessage::noteOff (1, 60) } });
    check (rms (out, 0, noteAt) < 1.0e-6, "silent before the note");
    check (rms (out, noteAt, (int) (0.2 * kRate)) > 0.05, "C5 plays chop 1");
    float maxDiff = 0.0f;
    for (int i = 100; i < 4000; ++i)
        maxDiff = std::max (maxDiff, std::abs (out.getSample (0, noteAt + i) - loop.buf.getSample (0, i)));
    check (maxDiff < 1.0e-3f, "chop 1 is a sample-exact copy of the source (max diff " + juce::String (maxDiff, 6) + ")");
    check (rms (out, noteAt + (int) (0.30 * kRate), (int) (0.2 * kRate)) < 1.0e-4, "stops when the note is released (gate mode)");

    out = render (*plugin, 0.4, { { 0, juce::MidiMessage::noteOn (1, 61, 1.0f) } });
    const auto chop2Start = (juce::int64) sliceState (st, 1)["start"];
    check (rms (out, 0, (int) (0.1 * kRate)) > 0.005, "C#5 plays chop 2 (starts " + juce::String (chop2Start / kRate, 3) + " s)");
    out = render (*plugin, 0.3, { { 0, juce::MidiMessage::noteOn (1, 60 + chops, 1.0f) } });
    check (rms (out, 0, out.getNumSamples()) < 1.0e-6, "notes past the last chop are silent");

    std::cout << "Per-chop edits: speed, reverse, pitch, label\n";
    st.getChildWithName ("Global").setProperty ("oneShot", true, nullptr);
    writeState (*plugin, st);
    check (waitForChops (*plugin, chops), "one-shot mode set");
    const double chop1Len = audibleSeconds (render (*plugin, 1.0, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } }), 0);
    sliceState (st, 0).setProperty ("speed", 2.0, nullptr);
    sliceState (st, 0).setProperty ("label", "kick", nullptr);
    sliceState (st, 1).setProperty ("reverse", true, nullptr);
    sliceState (st, 2).setProperty ("pitch", 12.0, nullptr);
    st.getChildWithName ("Global").setProperty ("oneShot", true, nullptr);
    writeState (*plugin, st);
    check (waitForChops (*plugin, chops), "settings restored");

    out = render (*plugin, 0.6, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } });
    const double spedUp = audibleSeconds (out, 0);
    std::cout << "  chop 1 sounds for " << chop1Len << " s, at 2x speed for " << spedUp << " s\n";
    check (std::abs (spedUp - chop1Len / 2.0) < chop1Len * 0.1, "2x speed halves the chop length");

    // chop 2 is a hi-hat over a kick tail; reversed, its loudest part is at the end
    const auto c2 = sliceState (st, 1);
    const int c2Len = (int) ((juce::int64) c2["end"] - (juce::int64) c2["start"]);
    out = render (*plugin, 0.5, { { 0, juce::MidiMessage::noteOn (1, 61, 1.0f) } });
    const double firstQuarter = rms (out, 0, c2Len / 4), lastQuarter = rms (out, c2Len * 3 / 4 - 200, c2Len / 4);
    check (lastQuarter > firstQuarter * 2.0, "reverse flips the chop (start " + juce::String (firstQuarter, 4) + ", end " + juce::String (lastQuarter, 4) + ")");

    out = render (*plugin, 0.5, { { 0, juce::MidiMessage::noteOn (1, 62, 1.0f) } });
    check (rms (out, 0, (int) (0.1 * kRate)) > 0.005, "pitched chop plays");

    std::cout << "Sync to project tempo\n";
    st = readState (*plugin);
    sliceState (st, 0).setProperty ("speed", 1.0, nullptr);
    st.getChildWithName ("Global").setProperty ("sync", true, nullptr);
    playHead.bpm = 186.0; // twice the sample's tempo
    render (*plugin, 0.05, {});
    writeState (*plugin, st);
    check (waitForChops (*plugin, chops), "settings restored");
    pump (600);
    render (*plugin, 0.05, {});
    out = render (*plugin, 0.6, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } });
    const double synced = audibleSeconds (out, 0);
    std::cout << "  project at 186 BPM, chop 1 sounds for " << synced << " s\n";
    check (std::abs (synced - chop1Len / 2.0) < chop1Len * 0.1, "chops follow the project tempo");

    std::cout << "Save and reopen the project with the original file gone\n";
    st = readState (*plugin);
    juce::MemoryBlock saved;
    plugin->getStateInformation (saved);
    plugin = nullptr;
    wavFile.deleteFile();

    FixedPlayHead playHead2;
    auto reopened = loadPlugin (pluginPath, playHead2);
    reopened->setStateInformation (saved.getData(), (int) saved.getSize());
    check (waitForChops (*reopened, chops), "project reopens with all chops");
    const auto st2 = readState (*reopened);
    check (sliceState (st2, 0)["label"].toString() == "kick", "labels survive");
    check ((bool) sliceState (st2, 1)["reverse"], "per-chop settings survive");
    check (std::abs ((double) st2["bpm"] - bpm) < 0.01, "tempo survives");
    out = render (*reopened, 0.4, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } });
    check (rms (out, 0, (int) (0.1 * kRate)) > 0.05, "audio plays from the embedded copy");

    reopened = nullptr;
    std::cout << "\n" << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
