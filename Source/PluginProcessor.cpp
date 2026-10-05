#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace choplab;

//==============================================================================
struct ChopLabProcessor::LoadJob
{
    int generation = 0;
    juce::File file;
    juce::String name;
    juce::MemoryBlock embedded; // FLAC saved in the project
    float embeddedScale = 1.0f;
    std::shared_ptr<const SampleData> sample; // already in memory (e.g. reversing)
    bool restore = false;
    Document saved; // settings and chops to restore (no audio)
};

struct ChopLabProcessor::LoadResult
{
    int generation = 0;
    juce::String error;
    std::shared_ptr<const SampleData> sample;
    std::shared_ptr<const AnalysisFeatures> features;
    std::vector<OnsetCandidate> onsets;
    TempoResult tempo;
    GridResult grid;
    KeyResult key;
    bool restore = false;
    Document saved;
};

namespace
{
constexpr double kMaxSampleSeconds = 10.0 * 60.0;
constexpr double kMaxEmbedSeconds = 10.0 * 60.0;

juce::String legalName (const juce::String& s)
{
    return juce::File::createLegalFileName (s.trim()).substring (0, 60);
}

// e.g. " [+3st 1.50x rev]", or empty when nothing is changed
juce::String editTag (const GlobalSettings& g, const SliceSettings* s, double hostRatio)
{
    const auto p = combine (g, s, hostRatio);
    juce::StringArray parts;
    if (std::abs (p.transpose) > 0.005)
        parts.add ((p.transpose > 0 ? "+" : "") + juce::String (p.transpose, std::abs (p.transpose - std::round (p.transpose)) < 0.005 ? 0 : 2) + "st");
    if (std::abs (p.speed - 1.0) > 0.0005)
        parts.add (juce::String (p.speed, 2) + "x");
    if (p.reverse)
        parts.add ("rev");
    const float gainDb = g.gainDb + (s != nullptr ? s->gainDb : 0.0f);
    if (std::abs (gainDb) > 0.05f)
        parts.add ((gainDb > 0 ? "+" : "") + juce::String (gainDb, 1) + "dB");
    return parts.isEmpty() ? juce::String() : " [" + parts.joinIntoString (" ") + "]";
}

bool writeWav (const juce::AudioBuffer<float>& buffer, double rate, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        return false;

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                   .withSampleRate (rate)
                                                   .withNumChannels (buffer.getNumChannels())
                                                   .withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}
} // namespace

//==============================================================================
ChopLabProcessor::ChopLabProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& p : playPositions)
        p = -1;
    startTimerHz (5);
}

ChopLabProcessor::~ChopLabProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    loadPool.removeAllJobs (true, 10000);
}

//==============================================================================
void ChopLabProcessor::prepareToPlay (double sampleRate, int)
{
    currentRate = sampleRate;
    preparedRate = sampleRate;
    for (auto& v : voices)
    {
        v.active = false;
        v.data = nullptr;
        v.slice = nullptr;
    }
}

bool ChopLabProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && layouts.getMainInputChannelSet().isDisabled();
}

void ChopLabProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    {
        const juce::SpinLock::ScopedTryLockType sl (publishLock);
        if (sl.isLocked() && published != audioData)
            audioData = published;
    }

    if (auto* ph = getPlayHead())
        if (auto position = ph->getPosition())
            if (auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
                hostBpm = *bpm;

    {
        int start1, size1, start2, size2;
        previewFifo.prepareToRead (previewFifo.getNumReady(), start1, size1, start2, size2);
        auto handle = [this] (int request)
        {
            if (request == kPreviewStop)
            {
                for (auto& v : voices)
                    if (v.active && v.preview)
                        releaseVoice (v, true);
            }
            else
            {
                startVoice (request == kPreviewFull ? -1 : request, -1, 1.0f, true);
            }
        };
        for (int i = 0; i < size1; ++i)
            handle (previewQueue[(size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i)
            handle (previewQueue[(size_t) (start2 + i)]);
        previewFifo.finishedRead (size1 + size2);
    }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, numSamples, meta.samplePosition);
        if (t > pos)
        {
            renderVoices (buffer, pos, t - pos);
            pos = t;
        }
        handleMidi (meta.getMessage());
    }
    if (pos < numSamples)
        renderVoices (buffer, pos, numSamples - pos);

    size_t reported = 0;
    for (const auto& v : voices)
    {
        if (reported >= playPositions.size())
            break;
        if (! v.active || v.slice == nullptr || v.slice->audio == nullptr)
            continue;
        const double frac = juce::jlimit (0.0, 1.0, v.pos / juce::jmax (1, v.slice->length));
        const auto span = (double) (v.slice->sourceEnd - v.slice->sourceStart);
        playPositions[reported++] = v.slice->sourceStart + (juce::int64) ((v.slice->reverse ? 1.0 - frac : frac) * span);
    }
    for (; reported < playPositions.size(); ++reported)
        playPositions[reported] = -1;
}

void ChopLabProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        if (audioData != nullptr)
        {
            const int index = m.getNoteNumber() - audioData->rootNote;
            if (index >= 0 && index < (int) audioData->slices.size())
                startVoice (index, m.getNoteNumber(), m.getFloatVelocity(), false);
        }
    }
    else if (m.isNoteOff())
    {
        for (auto& v : voices)
            if (v.active && ! v.preview && ! v.oneShot && ! v.releasing && v.note == m.getNoteNumber())
                releaseVoice (v, false);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            if (v.active)
                releaseVoice (v, true);
    }
}

void ChopLabProcessor::startVoice (int sliceIndex, int note, float velocity, bool preview)
{
    if (audioData == nullptr)
        return;

    const RenderedSlice* rs = nullptr;
    if (sliceIndex < 0)
        rs = &audioData->full;
    else if (sliceIndex < (int) audioData->slices.size())
        rs = &audioData->slices[(size_t) sliceIndex];
    if (rs == nullptr || rs->audio == nullptr || rs->length < 2)
        return;

    // Chops cut themselves off when retriggered; mono mode cuts everything.
    for (auto& v : voices)
        if (v.active && (audioData->mono || (preview && v.preview) || (! preview && ! v.preview && v.sliceIndex == sliceIndex)))
            releaseVoice (v, true);

    Voice* target = nullptr;
    for (auto& v : voices)
        if (! v.active)
        {
            target = &v;
            break;
        }
    if (target == nullptr)
        target = &*std::min_element (voices.begin(), voices.end(), [] (const Voice& a, const Voice& b) { return a.age < b.age; });

    auto& v = *target;
    v.data = audioData;
    v.slice = rs;
    v.sliceIndex = sliceIndex;
    v.note = note;
    v.preview = preview;
    v.oneShot = preview || audioData->oneShot;
    v.pos = 0.0;
    v.inc = audioData->sampleRate / currentRate;
    v.gain = rs->gain * audioData->masterGain * (preview ? 1.0f : velocity);
    const float attack = rs->attackMs * 0.001f * (float) currentRate;
    v.env = attack > 1.0f ? 0.0f : 1.0f;
    v.attackStep = attack > 1.0f ? 1.0f / attack : 1.0f;
    v.releaseStep = 1.0f / juce::jmax (1.0f, rs->releaseMs * 0.001f * (float) currentRate);
    v.releasing = false;
    v.active = true;
    v.age = ++voiceCounter;
}

void ChopLabProcessor::releaseVoice (Voice& v, bool fast)
{
    v.releasing = true;
    if (fast)
        v.releaseStep = juce::jmax (v.releaseStep, 1.0f / (0.004f * (float) currentRate));
}

void ChopLabProcessor::renderVoices (juce::AudioBuffer<float>& buffer, int start, int num)
{
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const auto& audio = *v.slice->audio;
        const int len = v.slice->length;
        const float* s0 = audio.getReadPointer (0) + v.slice->offset;
        const float* s1 = audio.getNumChannels() > 1 ? audio.getReadPointer (1) + v.slice->offset : s0;
        // Short fades at the chop edges so cutting mid-waveform never clicks
        const double fadeIn = 0.0003 * currentRate / v.inc, fadeOut = 0.003 * currentRate / v.inc;
        bool finished = false;

        for (int i = start; i < start + num; ++i)
        {
            const int idx = (int) v.pos;
            if (idx >= len - 1)
            {
                finished = true;
                break;
            }

            if (v.releasing)
            {
                v.env -= v.releaseStep;
                if (v.env <= 0.0f)
                {
                    finished = true;
                    break;
                }
            }
            else if (v.env < 1.0f)
            {
                v.env = juce::jmin (1.0f, v.env + v.attackStep);
            }

            const float frac = (float) (v.pos - idx);
            const float l = s0[idx] + frac * (s0[idx + 1] - s0[idx]);
            const float r = s1[idx] + frac * (s1[idx + 1] - s1[idx]);
            const double remaining = (double) (len - 1) - v.pos;
            const float edge = (float) juce::jmin (1.0, v.pos / fadeIn, remaining / fadeOut);
            const float g = v.gain * v.env * juce::jmax (0.0f, edge);
            if (right != nullptr)
            {
                left[i] += l * g;
                right[i] += r * g;
            }
            else
            {
                left[i] += 0.5f * (l + r) * g;
            }
            v.pos += v.inc;
        }

        if (finished)
        {
            v.active = false;
            v.slice = nullptr;
            v.data = nullptr;
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* ChopLabProcessor::createEditor()
{
    return new ChopLabEditor (*this);
}

//==============================================================================
bool ChopLabProcessor::canLoad (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg");
}

void ChopLabProcessor::loadFile (const juce::File& file)
{
    auto job = std::make_unique<LoadJob>();
    job->file = file;
    job->name = file.getFileNameWithoutExtension();
    startLoad (std::move (job));
}

void ChopLabProcessor::startLoad (std::unique_ptr<LoadJob> job)
{
    job->generation = ++loadGeneration;
    analysing = true;
    lastError = {};
    sendChangeMessage();

    loadPool.removeAllJobs (false, 0);
    std::shared_ptr<LoadJob> shared (std::move (job));
    loadPool.addJob ([this, shared]
    {
        auto result = runLoad (*shared);
        {
            const juce::ScopedLock sl (resultLock);
            pendingResult = std::move (result);
        }
        triggerAsyncUpdate();
    });
}

std::unique_ptr<ChopLabProcessor::LoadResult> ChopLabProcessor::runLoad (const LoadJob& job)
{
    auto result = std::make_unique<LoadResult>();
    result->generation = job.generation;
    result->restore = job.restore;
    result->saved = job.saved;

    auto sample = job.sample;
    if (sample == nullptr)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader;
        if (job.embedded.getSize() > 0)
            reader.reset (formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (job.embedded, false)));
        if (reader == nullptr && job.file.existsAsFile())
            reader.reset (formats.createReaderFor (job.file));

        if (reader == nullptr)
        {
            result->error = job.file.getFullPathName().isNotEmpty() && ! job.file.existsAsFile()
                                ? "Can't find " + job.file.getFullPathName()
                                : "Couldn't read " + job.name + " (unsupported or damaged file)";
            return result;
        }

        if (reader->lengthInSamples <= 0 || (double) reader->lengthInSamples / reader->sampleRate > kMaxSampleSeconds)
        {
            result->error = reader->lengthInSamples <= 0 ? job.name + " is empty" : job.name + " is longer than 10 minutes";
            return result;
        }

        auto data = std::make_shared<SampleData>();
        const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
        const int length = (int) reader->lengthInSamples;
        data->audio.setSize (channels, length);
        reader->read (&data->audio, 0, length, 0, true, channels > 1);
        if (job.embeddedScale > 0.0f && std::abs (job.embeddedScale - 1.0f) > 1.0e-6f)
            data->audio.applyGain (1.0f / job.embeddedScale);
        data->sampleRate = reader->sampleRate;
        data->name = job.name;
        data->file = job.file;
        sample = data;
    }

    result->sample = sample;
    auto features = std::make_shared<AnalysisFeatures> (computeFeatures (sample->audio, sample->sampleRate));
    result->onsets = detectOnsets (*features, sample->audio);
    result->tempo = estimateTempo (*features, result->onsets);
    result->grid = estimateGrid (*features, result->onsets, result->tempo.bpm, {}, true, result->tempo.loopDetected);
    result->key = estimateKey (*features);
    result->features = features;
    return result;
}

void ChopLabProcessor::handleAsyncUpdate()
{
    std::unique_ptr<LoadResult> result;
    {
        const juce::ScopedLock sl (resultLock);
        result = std::move (pendingResult);
    }
    if (result == nullptr || result->generation != loadGeneration)
        return;

    analysing = false;
    if (result->error.isNotEmpty())
    {
        lastError = result->error;
        sendChangeMessage();
        return;
    }

    Document d;
    d.sample = result->sample;
    d.features = result->features;
    d.onsets = std::move (result->onsets);
    d.detectedTempo = result->tempo;
    d.detectedGrid = result->grid;
    d.key = result->key;

    if (result->restore)
    {
        const auto& s = result->saved;
        d.bpm = s.bpm;
        d.timeSig = s.timeSig;
        d.meterIsAuto = s.meterIsAuto;
        d.downbeatSeconds = s.downbeatSeconds;
        d.reversed = s.reversed;
        d.global = s.global;
        d.chop = s.chop;
        d.slices = s.slices;
        std::vector<juce::int64> markers;
        for (const auto& slice : s.slices)
            markers.push_back (slice.start);
        applyMarkers (d, markers.empty() ? markersFor (d) : markers);
    }
    else
    {
        d.bpm = result->tempo.bpm;
        d.timeSig = result->grid.timeSig;
        d.downbeatSeconds = result->grid.downbeatSeconds;
        d.global = document.global; // keep root note, trigger mode etc., reset the sound shaping
        d.global.pitch = 0.0f;
        d.global.speed = 1.0f;
        d.global.gainDb = 0.0f;
        d.chop = document.chop;
        applyMarkers (d, markersFor (d));
    }

    {
        const juce::ScopedLock sl (docLock);
        document = std::move (d);
    }
    clearHistory(); // positions in old snapshots don't apply to new (or reversed) audio
    selectedSlice = document.slices.empty() ? -1 : 0;
    lastError = {};
    requestRender();
    sendChangeMessage();
}

//==============================================================================
std::vector<juce::int64> ChopLabProcessor::markersFor (const Document& d) const
{
    switch (d.chop.mode)
    {
        case ChopMode::transients:
            return transientMarkers (d.onsets, d.chop.sensitivity, d.chop.minLengthMs, d.sampleRate(), d.length());
        case ChopMode::grid:
            return gridMarkers (d.bpm, d.downbeatSeconds, d.timeSig, d.chop.gridBeats, d.sampleRate(), d.length());
        case ChopMode::manual:
        default:
        {
            std::vector<juce::int64> m;
            for (const auto& s : d.slices)
                m.push_back (s.start);
            if (m.empty())
                m.push_back (0);
            return m;
        }
    }
}

void ChopLabProcessor::refreshGrid()
{
    if (document.features == nullptr)
        return;
    const double beats = document.sample->lengthSeconds() * document.bpm / 60.0;
    const bool looksLikeLoop = std::abs (beats - std::round (beats)) < 0.02 && std::round (beats) >= 4.0;
    const auto g = estimateGrid (*document.features, document.onsets, document.bpm, document.timeSig, document.meterIsAuto, looksLikeLoop);
    document.downbeatSeconds = g.downbeatSeconds;
    if (document.meterIsAuto)
    {
        document.timeSig = g.timeSig;
        document.detectedGrid = g;
    }
}

void ChopLabProcessor::setBpm (double bpm)
{
    if (! document.hasSample())
        return;
    checkpoint (EditKind::tempo);
    {
        const juce::ScopedLock sl (docLock);
        document.bpm = juce::jlimit (20.0, 400.0, bpm);
        refreshGrid();
        if (document.chop.mode == ChopMode::grid)
            applyMarkers (document, markersFor (document));
    }
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setTimeSignature (TimeSignature ts, bool automatic)
{
    checkpoint (EditKind::timeSig);
    {
        const juce::ScopedLock sl (docLock);
        document.meterIsAuto = automatic;
        if (! automatic)
            document.timeSig = ts;
        refreshGrid();
        if (document.chop.mode == ChopMode::grid)
            applyMarkers (document, markersFor (document));
    }
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setDownbeat (double seconds)
{
    checkpoint (EditKind::downbeat);
    {
        const juce::ScopedLock sl (docLock);
        document.downbeatSeconds = juce::jmax (0.0, seconds);
        if (document.chop.mode == ChopMode::grid)
            applyMarkers (document, markersFor (document));
    }
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setChopSettings (const ChopSettings& c, bool rechopNow)
{
    checkpoint (EditKind::chop);
    {
        const juce::ScopedLock sl (docLock);
        document.chop = c;
    }
    if (rechopNow)
        rechop();
    else
        sendChangeMessage();
}

void ChopLabProcessor::rechop()
{
    if (! document.hasSample())
        return;
    {
        const juce::ScopedLock sl (docLock);
        applyMarkers (document, markersFor (document));
    }
    selectedSlice = juce::jlimit (-1, (int) document.slices.size() - 1, selectedSlice);
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::moveMarker (int index, juce::int64 newStart, bool finished)
{
    auto& slices = document.slices;
    if (index <= 0 || index >= (int) slices.size())
        return;
    checkpoint (EditKind::marker, index);
    {
        const juce::ScopedLock sl (docLock);
        const auto minGap = (juce::int64) (document.sampleRate() * 0.005);
        newStart = juce::jlimit (slices[(size_t) index - 1].start + minGap, slices[(size_t) index].end - minGap, newStart);
        slices[(size_t) index - 1].end = newStart;
        slices[(size_t) index].start = newStart;
        if (finished)
        {
            slices[(size_t) index - 1].info = {};
            slices[(size_t) index].info = {};
            describeSlices (document);
        }
    }
    if (finished)
        requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::addMarker (juce::int64 position)
{
    auto& slices = document.slices;
    if (! document.hasSample() || (int) slices.size() >= kMaxSlices)
        return;
    const auto minGap = (juce::int64) (document.sampleRate() * 0.005);
    for (size_t i = 0; i < slices.size(); ++i)
    {
        auto& s = slices[i];
        if (position <= s.start || position >= s.end)
            continue;
        if (position - s.start < minGap || s.end - position < minGap)
            return;
        checkpoint (EditKind::addMarker);
        {
            const juce::ScopedLock sl (docLock);
            Slice added;
            added.start = position;
            added.end = s.end;
            added.settings = s.settings;
            added.settings.label = {};
            s.end = position;
            s.info = {};
            slices.insert (slices.begin() + (std::ptrdiff_t) i + 1, added);
            describeSlices (document);
        }
        selectedSlice = (int) i + 1;
        requestRender();
        sendChangeMessage();
        return;
    }
}

void ChopLabProcessor::removeMarker (int index)
{
    auto& slices = document.slices;
    if (index <= 0 || index >= (int) slices.size())
        return;
    checkpoint (EditKind::removeMarker);
    {
        const juce::ScopedLock sl (docLock);
        slices[(size_t) index - 1].end = slices[(size_t) index].end;
        slices[(size_t) index - 1].info = {};
        slices.erase (slices.begin() + index);
        describeSlices (document);
    }
    if (selectedSlice >= index)
        selectedSlice = juce::jmax (0, selectedSlice - 1);
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setSliceSettings (int index, const SliceSettings& s)
{
    if (index < 0 || index >= (int) document.slices.size())
        return;
    checkpoint (EditKind::slice, index);
    {
        const juce::ScopedLock sl (docLock);
        document.slices[(size_t) index].settings = s;
    }
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setGlobalSettings (const GlobalSettings& g)
{
    checkpoint (EditKind::global);
    {
        const juce::ScopedLock sl (docLock);
        document.global = g;
    }
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::setReversed (bool shouldBeReversed)
{
    if (! document.hasSample() || shouldBeReversed == document.reversed || analysing)
        return;

    auto reversedSample = std::make_shared<SampleData> (*document.sample);
    reversedSample->audio.reverse (0, reversedSample->audio.getNumSamples());

    // Mirror everything: chops keep their settings, bar lines stay bar lines.
    auto job = std::make_unique<LoadJob>();
    job->sample = reversedSample;
    job->restore = true;
    auto& s = job->saved;
    s.bpm = document.bpm;
    s.timeSig = document.timeSig;
    s.meterIsAuto = document.meterIsAuto;
    s.reversed = shouldBeReversed;
    s.global = document.global;
    s.chop = document.chop;

    const double lengthSeconds = document.sample->lengthSeconds();
    const double barSeconds = document.timeSig.quarterBeatsPerBar() * 60.0 / document.bpm;
    const double mirrored = lengthSeconds - document.downbeatSeconds;
    s.downbeatSeconds = mirrored - std::floor (mirrored / barSeconds) * barSeconds;

    const auto length = document.length();
    for (auto it = document.slices.rbegin(); it != document.slices.rend(); ++it)
    {
        Slice m;
        m.start = length - it->end;
        m.end = length - it->start;
        m.settings = it->settings;
        s.slices.push_back (m);
    }
    if (selectedSlice >= 0)
        selectedSlice = (int) document.slices.size() - 1 - selectedSlice;
    startLoad (std::move (job));
}

//==============================================================================
ChopLabProcessor::Snapshot ChopLabProcessor::snapshot() const
{
    return { document.slices, document.bpm, document.downbeatSeconds, document.timeSig, document.meterIsAuto, document.chop, document.global, selectedSlice };
}

void ChopLabProcessor::restore (const Snapshot& s)
{
    {
        const juce::ScopedLock sl (docLock);
        document.slices = s.slices;
        document.bpm = s.bpm;
        document.downbeatSeconds = s.downbeatSeconds;
        document.timeSig = s.timeSig;
        document.meterIsAuto = s.meterIsAuto;
        document.chop = s.chop;
        document.global = s.global;
    }
    selectedSlice = juce::jlimit (-1, (int) document.slices.size() - 1, s.selected);
    lastEditKind = 0;
    requestRender();
    sendChangeMessage();
}

void ChopLabProcessor::checkpoint (EditKind kind, int detail)
{
    if (! document.hasSample())
        return;
    const int key = (int) kind + detail;
    const auto now = juce::Time::getMillisecondCounter();
    const bool sameGesture = key == lastEditKind && now - lastEditTime < 1000;
    lastEditKind = key;
    lastEditTime = now;
    if (sameGesture)
        return; // knob drags and marker drags become one undo step

    undoStack.push_back (snapshot());
    if (undoStack.size() > 100)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void ChopLabProcessor::clearHistory()
{
    undoStack.clear();
    redoStack.clear();
    lastEditKind = 0;
}

void ChopLabProcessor::undo()
{
    if (undoStack.empty() || analysing)
        return;
    redoStack.push_back (snapshot());
    const auto s = undoStack.back();
    undoStack.pop_back();
    restore (s);
}

void ChopLabProcessor::redo()
{
    if (redoStack.empty() || analysing)
        return;
    undoStack.push_back (snapshot());
    const auto s = redoStack.back();
    redoStack.pop_back();
    restore (s);
}

void ChopLabProcessor::selectSlice (int index)
{
    selectedSlice = juce::jlimit (-1, (int) document.slices.size() - 1, index);
    sendChangeMessage();
}

//==============================================================================
void ChopLabProcessor::previewSlice (int index)
{
    int start1, size1, start2, size2;
    previewFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
        previewQueue[(size_t) start1] = index;
    previewFifo.finishedWrite (size1);
}

void ChopLabProcessor::previewFull()
{
    previewSlice (kPreviewFull);
}

void ChopLabProcessor::stopPreview()
{
    previewSlice (kPreviewStop);
}

void ChopLabProcessor::getPlayingPositions (std::vector<juce::int64>& out) const
{
    out.clear();
    for (const auto& p : playPositions)
        if (const auto v = p.load(); v >= 0)
            out.push_back (v);
}

//==============================================================================
double ChopLabProcessor::hostTempoRatio() const
{
    return document.bpm > 0.0 ? juce::jlimit (0.25, 4.0, hostBpm.load() / document.bpm) : 1.0;
}

void ChopLabProcessor::requestRender()
{
    RenderRequest r;
    r.sample = document.sample;
    r.slices = document.slices;
    r.global = document.global;
    r.hostTempoRatio = hostTempoRatio();
    r.targetRate = preparedRate > 0.0 ? preparedRate.load() : 44100.0;
    renderedRate = r.targetRate;
    renderedHostBpm = hostBpm;
    renderThread.request (std::move (r));
}

void ChopLabProcessor::publish (PlaybackData::Ptr data)
{
    PlaybackData::Ptr old;
    {
        const juce::SpinLock::ScopedLockType sl (publishLock);
        old = published;
        published = data;
    }
    if (old != nullptr)
    {
        const juce::ScopedLock sl (retiredLock);
        retired.add (old);
    }
    releaseRetired();
}

void ChopLabProcessor::releaseRetired()
{
    // Old playback data is only freed here, never on the audio thread.
    const juce::ScopedLock sl (retiredLock);
    for (int i = retired.size(); --i >= 0;)
        if (retired.getObjectPointerUnchecked (i)->getReferenceCount() == 1)
            retired.remove (i);
}

void ChopLabProcessor::timerCallback()
{
    releaseRetired();
    if (! document.hasSample())
        return;
    const bool rateChanged = preparedRate > 0.0 && std::abs (preparedRate - renderedRate) > 0.5;
    const bool tempoChanged = document.global.syncToHost && std::abs (hostBpm - renderedHostBpm) > 0.01;
    if (rateChanged || tempoChanged)
        requestRender();
}

//==============================================================================
juce::File ChopLabProcessor::getDragFolder() const
{
    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                      .getChildFile ("ChopLab")
                      .getChildFile (document.sample != nullptr ? legalName (document.sample->name) : juce::String ("Untitled"));
    folder.createDirectory();
    return folder;
}

juce::AudioBuffer<float> ChopLabProcessor::renderForExport (int index) const
{
    if (! document.hasSample() || index >= (int) document.slices.size())
        return {};
    const Slice* slice = index >= 0 ? &document.slices[(size_t) index] : nullptr;
    const auto p = combine (document.global, slice != nullptr ? &slice->settings : nullptr, hostTempoRatio());
    auto buffer = renderSegment (*document.sample, slice != nullptr ? slice->start : 0, slice != nullptr ? slice->end : document.length(), p,
                                 document.sampleRate());
    float gain = juce::Decibels::decibelsToGain (document.global.gainDb);
    if (slice != nullptr)
        gain *= juce::Decibels::decibelsToGain (slice->settings.gainDb);
    buffer.applyGain (gain);
    return buffer;
}

juce::File ChopLabProcessor::renderSliceToFile (int index, const juce::File& folder) const
{
    if (index < 0 || index >= (int) document.slices.size())
        return {};
    const auto& settings = document.slices[(size_t) index].settings;
    auto name = legalName (document.sample->name) + " - " + juce::String (index + 1).paddedLeft ('0', 2);
    if (settings.label.isNotEmpty())
        name << " " << legalName (settings.label);
    name << legalName (editTag (document.global, &settings, hostTempoRatio()));
    const auto file = folder.getChildFile (name + ".wav");
    if (writeWav (renderForExport (index), document.sampleRate(), file))
        return file;
    return file.existsAsFile() ? file : juce::File(); // same name means same content; FL may have it open
}

juce::File ChopLabProcessor::renderFullToFile (const juce::File& folder) const
{
    if (! document.hasSample())
        return {};
    const auto file = folder.getChildFile (legalName (document.sample->name) + legalName (editTag (document.global, nullptr, hostTempoRatio())) + ".wav");
    if (writeWav (renderForExport (-1), document.sampleRate(), file))
        return file;
    return file.existsAsFile() ? file : juce::File();
}

int ChopLabProcessor::exportAllSlices (const juce::File& folder) const
{
    int written = 0;
    for (int i = 0; i < (int) document.slices.size(); ++i)
        if (renderSliceToFile (i, folder).existsAsFile())
            ++written;
    return written;
}

juce::File ChopLabProcessor::writeMidiPattern (const juce::File& folder) const
{
    if (! document.hasSample() || document.slices.empty())
        return {};

    constexpr int ppq = 960;
    const double bpm = document.bpm;
    const double barBeats = document.timeSig.quarterBeatsPerBar();

    // Shift so bar 1 of the sample lands on a bar line in the piano roll.
    const double downbeatBeats = document.downbeatSeconds * bpm / 60.0;
    const double shift = downbeatBeats > 1.0e-6 ? std::ceil (downbeatBeats / barBeats - 1.0e-6) * barBeats - downbeatBeats : 0.0;

    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::llround (60000000.0 / bpm)), 0);
    const int den = document.timeSig.denominator;
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (document.timeSig.numerator, den), 0);

    for (int i = 0; i < (int) document.slices.size(); ++i)
    {
        const int note = document.noteForSlice (i);
        if (note < 0)
            continue;
        const auto& s = document.slices[(size_t) i];
        const double startBeat = document.secondsAt (s.start) * bpm / 60.0 + shift;
        const double lengthBeats = document.lengthInBeats (s) / juce::jmax (0.01, (double) s.settings.speed * document.global.speed);
        const double t0 = std::round (startBeat * ppq);
        const double t1 = juce::jmax (t0 + 1.0, std::round ((startBeat + lengthBeats) * ppq) - 1.0);
        track.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), t0);
        track.addEvent (juce::MidiMessage::noteOff (1, note), t1);
    }
    track.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ppq);
    midi.addTrack (track);

    const auto file = folder.getChildFile (legalName (document.sample->name) + " - chop pattern.mid");
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk() || ! midi.writeTo (out))
        return {};
    return file;
}

//==============================================================================
std::shared_ptr<const ChopLabProcessor::EncodedAudio> ChopLabProcessor::encodedAudio() const
{
    const juce::ScopedLock sl (encodedLock);
    if (encodedFor == document.sample.get() && encodedCache != nullptr)
        return encodedCache;

    encodedFor = document.sample.get();
    encodedCache = nullptr;
    if (! document.hasSample() || document.sample->lengthSeconds() > kMaxEmbedSeconds)
        return nullptr;

    auto encoded = std::make_shared<EncodedAudio>();
    const auto& audio = document.sample->audio;
    const float peak = audio.getMagnitude (0, audio.getNumSamples());
    encoded->scale = peak > 1.0f ? 1.0f / peak : 1.0f;

    juce::AudioBuffer<float> scaled (audio);
    scaled.applyGain (encoded->scale);

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (encoded->flac, false);
    juce::FlacAudioFormat flac;
    {
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                        .withSampleRate (document.sample->sampleRate)
                                                        .withNumChannels (audio.getNumChannels())
                                                        .withBitsPerSample (24));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (scaled, 0, scaled.getNumSamples()))
            return nullptr;
    }
    encodedCache = encoded;
    return encodedCache;
}

juce::ValueTree ChopLabProcessor::toValueTree() const
{
    juce::ValueTree t ("ChopLab");
    t.setProperty ("version", 1, nullptr);
    const auto& d = document;

    if (d.hasSample())
    {
        t.setProperty ("file", d.sample->file.getFullPathName(), nullptr);
        t.setProperty ("name", d.sample->name, nullptr);
        if (auto encoded = encodedAudio())
        {
            t.setProperty ("audio", juce::var (encoded->flac), nullptr);
            t.setProperty ("audioScale", encoded->scale, nullptr);
        }
    }

    t.setProperty ("bpm", d.bpm, nullptr);
    t.setProperty ("downbeat", d.downbeatSeconds, nullptr);
    t.setProperty ("tsNum", d.timeSig.numerator, nullptr);
    t.setProperty ("tsDen", d.timeSig.denominator, nullptr);
    t.setProperty ("meterAuto", d.meterIsAuto, nullptr);
    t.setProperty ("reversed", d.reversed, nullptr);

    juce::ValueTree g ("Global");
    g.setProperty ("pitch", d.global.pitch, nullptr);
    g.setProperty ("speed", d.global.speed, nullptr);
    g.setProperty ("keepPitch", d.global.keepPitch, nullptr);
    g.setProperty ("sync", d.global.syncToHost, nullptr);
    g.setProperty ("gain", d.global.gainDb, nullptr);
    g.setProperty ("root", d.global.rootNote, nullptr);
    g.setProperty ("oneShot", d.global.oneShot, nullptr);
    g.setProperty ("mono", d.global.mono, nullptr);
    t.appendChild (g, nullptr);

    juce::ValueTree c ("Chop");
    c.setProperty ("mode", (int) d.chop.mode, nullptr);
    c.setProperty ("sensitivity", d.chop.sensitivity, nullptr);
    c.setProperty ("gridBeats", d.chop.gridBeats, nullptr);
    c.setProperty ("minLength", d.chop.minLengthMs, nullptr);
    t.appendChild (c, nullptr);

    juce::ValueTree slices ("Slices");
    for (const auto& s : d.slices)
    {
        juce::ValueTree st ("Slice");
        st.setProperty ("start", s.start, nullptr);
        st.setProperty ("end", s.end, nullptr);
        st.setProperty ("label", s.settings.label, nullptr);
        st.setProperty ("pitch", s.settings.pitch, nullptr);
        st.setProperty ("speed", s.settings.speed, nullptr);
        st.setProperty ("keepPitch", s.settings.keepPitch, nullptr);
        st.setProperty ("reverse", s.settings.reverse, nullptr);
        st.setProperty ("gain", s.settings.gainDb, nullptr);
        st.setProperty ("attack", s.settings.attackMs, nullptr);
        st.setProperty ("release", s.settings.releaseMs, nullptr);
        slices.appendChild (st, nullptr);
    }
    t.appendChild (slices, nullptr);
    return t;
}

void ChopLabProcessor::readSettings (const juce::ValueTree& t, Document& d)
{
    d.bpm = juce::jlimit (20.0, 400.0, (double) t.getProperty ("bpm", 120.0));
    d.downbeatSeconds = t.getProperty ("downbeat", 0.0);
    d.timeSig.numerator = juce::jlimit (1, 32, (int) t.getProperty ("tsNum", 4));
    d.timeSig.denominator = juce::jlimit (1, 32, (int) t.getProperty ("tsDen", 4));
    d.meterIsAuto = t.getProperty ("meterAuto", true);
    d.reversed = t.getProperty ("reversed", false);

    if (auto g = t.getChildWithName ("Global"); g.isValid())
    {
        d.global.pitch = g.getProperty ("pitch", 0.0f);
        d.global.speed = juce::jlimit (0.25f, 4.0f, (float) g.getProperty ("speed", 1.0f));
        d.global.keepPitch = g.getProperty ("keepPitch", true);
        d.global.syncToHost = g.getProperty ("sync", false);
        d.global.gainDb = g.getProperty ("gain", 0.0f);
        d.global.rootNote = juce::jlimit (0, 127, (int) g.getProperty ("root", 60));
        d.global.oneShot = g.getProperty ("oneShot", false);
        d.global.mono = g.getProperty ("mono", false);
    }

    if (auto c = t.getChildWithName ("Chop"); c.isValid())
    {
        d.chop.mode = (ChopMode) juce::jlimit (0, 2, (int) c.getProperty ("mode", 0));
        d.chop.sensitivity = c.getProperty ("sensitivity", 0.5f);
        d.chop.gridBeats = c.getProperty ("gridBeats", 1.0);
        d.chop.minLengthMs = c.getProperty ("minLength", 70.0f);
    }

    d.slices.clear();
    for (const auto& st : t.getChildWithName ("Slices"))
    {
        Slice s;
        s.start = (juce::int64) st.getProperty ("start", 0);
        s.end = (juce::int64) st.getProperty ("end", 0);
        s.settings.label = st.getProperty ("label", "").toString();
        s.settings.pitch = st.getProperty ("pitch", 0.0f);
        s.settings.speed = juce::jlimit (0.25f, 4.0f, (float) st.getProperty ("speed", 1.0f));
        s.settings.keepPitch = st.getProperty ("keepPitch", true);
        s.settings.reverse = st.getProperty ("reverse", false);
        s.settings.gainDb = st.getProperty ("gain", 0.0f);
        s.settings.attackMs = st.getProperty ("attack", 0.0f);
        s.settings.releaseMs = st.getProperty ("release", 15.0f);
        d.slices.push_back (s);
    }
}

void ChopLabProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree t;
    {
        const juce::ScopedLock sl (docLock);
        t = toValueTree();
    }
    juce::MemoryOutputStream out (dest, false);
    t.writeToStream (out);
}

void ChopLabProcessor::setStateInformation (const void* data, int size)
{
    const auto t = juce::ValueTree::readFromData (data, (size_t) size);
    if (! t.isValid() || ! t.hasType ("ChopLab"))
        return;

    auto job = std::make_unique<LoadJob>();
    readSettings (t, job->saved);
    // Restore chops and tempo exactly as saved; a state that only names a file gets a fresh analysis.
    job->restore = t.hasProperty ("bpm") && ! job->saved.slices.empty();

    {
        const juce::ScopedLock sl (docLock);
        document.global = job->saved.global;
        document.chop = job->saved.chop;
    }

    const auto path = t.getProperty ("file", "").toString();
    if (juce::File::isAbsolutePath (path))
        job->file = juce::File (path);
    job->name = t.getProperty ("name", job->file.getFileNameWithoutExtension()).toString();
    if (const auto* block = t.getProperty ("audio").getBinaryData())
    {
        job->embedded = *block;
        job->embeddedScale = t.getProperty ("audioScale", 1.0f);
    }

    if (job->embedded.getSize() == 0 && path.isEmpty())
    {
        sendChangeMessage(); // settings only, no sample yet
        return;
    }
    startLoad (std::move (job));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChopLabProcessor();
}
