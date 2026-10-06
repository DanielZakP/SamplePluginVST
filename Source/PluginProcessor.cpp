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
    std::shared_ptr<const SampleData> sample; // the mix, already in memory (e.g. reversing)
    bool restore = false;
    Document saved; // settings and chops to restore (no audio)

    std::shared_ptr<const StemSet> stems;      // already in memory (switching source, reversing)
    std::array<juce::File, kNumStems> stemFiles; // or saved next to a project, to read back
    std::array<juce::int64, kNumStems> stemHashes {};
    int source = -1;
    // Only the playing audio changes: tempo, key and the undo history stay, and lyrics and
    // separation carry on.
    bool switchingSource = false;
    TempoResult tempo;
    GridResult grid;
    KeyResult key;
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
    std::shared_ptr<const StemSet> stems;
    int source = -1;
    bool switchingSource = false;
    juce::String note; // a problem worth mentioning that didn't stop the load
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

bool writeWav (const juce::AudioBuffer<float>& buffer, double rate, const juce::File& file, int bits = 24)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        return false;

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                   .withSampleRate (rate)
                                                   .withNumChannels (buffer.getNumChannels())
                                                   .withBitsPerSample (bits));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

// Fingerprint of a stem's audio, so a project only takes back stem files that still hold its stems.
juce::int64 audioHash (const juce::AudioBuffer<float>& audio)
{
    juce::uint64 h = 14695981039346656037ull;
    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        const auto* d = audio.getReadPointer (c);
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            juce::uint32 bits;
            std::memcpy (&bits, d + i, sizeof (bits));
            h = (h ^ bits) * 1099511628211ull;
        }
    }
    return (juce::int64) (h & 0x7fffffffffffffffull);
}

juce::AudioBuffer<float> forwards (const SampleData& data, bool reversed)
{
    juce::AudioBuffer<float> audio (data.audio);
    if (reversed)
        audio.reverse (0, audio.getNumSamples());
    return audio;
}

std::shared_ptr<SampleData> stemSample (juce::AudioBuffer<float> audio, const SampleData& mix, int stem, const juce::File& file)
{
    auto data = std::make_shared<SampleData>();
    data->audio = std::move (audio);
    data->sampleRate = mix.sampleRate;
    data->name = mix.name + " (" + stemName (stem) + ")";
    data->file = file;
    return data;
}

// Saves the stems (forwards, as 32-bit float so nothing clips) and keeps them in memory the way
// the mix plays. A stem that can't be saved still works; it's written again when dragged out.
std::shared_ptr<const StemSet> makeStemSet (StemsResult& result, const std::shared_ptr<const SampleData>& mix, bool reversed,
                                            const juce::File& folder)
{
    auto set = std::make_shared<StemSet>();
    set->mix = mix;
    folder.createDirectory();
    for (int i = 0; i < kNumStems; ++i)
    {
        auto& audio = result.stems[(size_t) i];
        set->hashes[(size_t) i] = audioHash (audio);
        auto file = folder.getChildFile (legalName (mix->name) + " - " + stemName (i) + ".wav");
        if (! writeWav (audio, mix->sampleRate, file, 32))
        {
            file = file.getNonexistentSibling();
            if (! writeWav (audio, mix->sampleRate, file, 32))
                file = juce::File();
        }
        set->files[(size_t) i] = file;
        if (reversed)
            audio.reverse (0, audio.getNumSamples());
        set->stems[(size_t) i] = stemSample (std::move (audio), *mix, i, file);
    }
    return set;
}

std::shared_ptr<const StemSet> readStems (const std::array<juce::File, kNumStems>& files, const std::array<juce::int64, kNumStems>& hashes,
                                          const std::shared_ptr<const SampleData>& mix, bool reversed)
{
    auto set = std::make_shared<StemSet>();
    set->mix = mix;
    set->files = files;
    set->hashes = hashes;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const int length = mix->audio.getNumSamples();
    for (int i = 0; i < kNumStems; ++i)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (files[(size_t) i].existsAsFile() ? formats.createReaderFor (files[(size_t) i]) : nullptr);
        if (reader == nullptr || reader->lengthInSamples != length || std::abs (reader->sampleRate - mix->sampleRate) > 0.5)
            return nullptr;
        juce::AudioBuffer<float> audio (mix->audio.getNumChannels(), length);
        reader->read (&audio, 0, length, 0, true, true);
        if (audioHash (audio) != hashes[(size_t) i])
            return nullptr;
        if (reversed)
            audio.reverse (0, length);
        set->stems[(size_t) i] = stemSample (std::move (audio), *mix, i, files[(size_t) i]);
    }
    return set;
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
    ++lyricsGeneration; // makes a running download or transcription stop at its next check
    ++stemsGeneration;
    lyricsPool.removeAllJobs (true, 30000);
    stemsPool.removeAllJobs (true, 60000);
    loadPool.removeAllJobs (true, 10000);
}

//==============================================================================
void ChopLabProcessor::prepareToPlay (double sampleRate, int)
{
    currentRate = sampleRate;
    preparedRate = sampleRate;
    bank.reset();
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
        auto handle = [this] (const PreviewRequest& r)
        {
            const int request = r.index;
            if (request == kPreviewStop)
            {
                for (auto& v : bank.voices)
                    if (v.active && v.preview)
                        releaseVoice (v, true);
            }
            else
            {
                startVoice (request == kPreviewFull ? -1 : request, -1, 1.0f, true, false, r.startSeconds);
            }
        };
        for (int i = 0; i < size1; ++i)
            handle (previewQueue[(size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i)
            handle (previewQueue[(size_t) (start2 + i)]);
        previewFifo.finishedRead (size1 + size2);
    }

    {
        const juce::SpinLock::ScopedTryLockType sl (patternLock);
        if (sl.isLocked() && publishedPattern != audioPattern)
            audioPattern = publishedPattern;
    }

    // Host MIDI and whatever the built-in pattern plays in this block, in time order.
    addPatternEvents (numSamples);
    int pos = 0;
    size_t next = 0;
    auto renderUpTo = [&] (int t)
    {
        if (t > pos)
        {
            renderVoices (buffer, pos, t - pos);
            pos = t;
        }
    };
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, numSamples, meta.samplePosition);
        for (; next < numPatternEvents && patternEvents[next].at <= t; ++next)
        {
            renderUpTo (patternEvents[next].at);
            handlePatternEvent (patternEvents[next]);
        }
        renderUpTo (t);
        handleMidi (meta.getMessage());
    }
    for (; next < numPatternEvents; ++next)
    {
        renderUpTo (patternEvents[next].at);
        handlePatternEvent (patternEvents[next]);
    }
    renderUpTo (numSamples);

    size_t reported = 0;
    for (const auto& v : bank.voices)
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

void ChopLabProcessor::addPatternEvents (int numSamples)
{
    numPatternEvents = 0;
    const bool playing = patternPlaying.load();
    if (playing && ! patternWasPlaying)
        patternBeat = 0.0;
    if (! playing && patternWasPlaying)
        for (auto& v : bank.voices)
            if (v.active && v.fromPattern)
                releaseVoice (v, true);
    patternWasPlaying = playing;

    if (! playing || audioPattern == nullptr || audioData == nullptr || audioPattern->lengthBeats <= 0.0)
        return;

    const double beatsPerSample = hostBpm.load() / 60.0 / currentRate;
    const double span = numSamples * beatsPerSample;

    const int numChops = (int) audioData->slices.size();
    forEachPatternEvent (audioPattern->notes, audioPattern->lengthBeats, patternBeat, span, [&] (size_t index, double beats, bool on)
    {
        const auto& n = audioPattern->notes[index];
        if (numPatternEvents < patternEvents.size() && n.chop >= 0 && n.chop < numChops)
            patternEvents[numPatternEvents++] = { juce::jlimit (0, numSamples - 1, (int) (beats / beatsPerSample)), n.chop, n.velocity, n.offset, on };
    });
    // Note-offs before note-ons at the same moment, so a repeated chop isn't cut by its own previous note.
    std::sort (patternEvents.begin(), patternEvents.begin() + (std::ptrdiff_t) numPatternEvents,
               [] (const PatternEvent& a, const PatternEvent& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); });

    patternBeat = std::fmod (patternBeat + span, audioPattern->lengthBeats);
    patternPosition = patternBeat;
}

void ChopLabProcessor::handlePatternEvent (const PatternEvent& e)
{
    if (e.on)
    {
        startVoice (e.chop, -1, e.velocity, false, true, e.offset);
        return;
    }
    for (auto& v : bank.voices)
        if (v.active && v.fromPattern && ! v.oneShot && ! v.releasing && v.sliceIndex == e.chop)
            releaseVoice (v, false);
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
        for (auto& v : bank.voices)
            if (v.active && ! v.preview && ! v.oneShot && ! v.releasing && ! v.fromPattern && v.note == m.getNoteNumber())
                releaseVoice (v, false);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : bank.voices)
            if (v.active)
                releaseVoice (v, true);
    }
}

void ChopLabProcessor::startVoice (int sliceIndex, int note, float velocity, bool preview, bool fromPattern, double startSeconds)
{
    bank.start (audioData, sliceIndex, note, velocity, preview, fromPattern, startSeconds, currentRate);
}

void ChopLabProcessor::releaseVoice (Voice& v, bool fast)
{
    VoiceBank::release (v, fast, currentRate);
}

void ChopLabProcessor::renderVoices (juce::AudioBuffer<float>& buffer, int start, int num)
{
    bank.render (buffer, start, num, currentRate);
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
    if (! job->switchingSource)
    {
        cancelLyrics(); // they'd belong to the old audio
        cancelStems();
    }
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

    // Stems: handed over (switching source, reversing) or read back from next to the project.
    auto stems = job.stems;
    if (stems == nullptr && job.stemFiles[0] != juce::File())
    {
        stems = readStems (job.stemFiles, job.stemHashes, sample, job.saved.reversed);
        if (stems == nullptr)
            result->note = "The stems separated for this project are gone or changed on disk. Separate again to get them back.";
    }
    result->stems = stems;
    result->source = stems != nullptr ? juce::jlimit (-1, kNumStems - 1, job.source) : -1;
    result->switchingSource = job.switchingSource;
    const auto playing = result->source >= 0 ? stems->stems[(size_t) result->source] : sample;
    result->sample = playing;

    // Chop points and chop descriptions come from what plays; tempo, meter and key from the mix.
    auto features = std::make_shared<AnalysisFeatures> (computeFeatures (playing->audio, playing->sampleRate));
    result->onsets = detectOnsets (*features, playing->audio);
    if (job.switchingSource)
    {
        result->tempo = job.tempo;
        result->grid = job.grid;
        result->key = job.key;
    }
    else
    {
        auto mixFeatures = playing == sample ? features : std::make_shared<AnalysisFeatures> (computeFeatures (sample->audio, sample->sampleRate));
        const auto mixOnsets = playing == sample ? result->onsets : detectOnsets (*mixFeatures, sample->audio);
        result->tempo = estimateTempo (*mixFeatures, mixOnsets);
        result->grid = estimateGrid (*mixFeatures, mixOnsets, result->tempo.bpm, {}, true, result->tempo.loopDetected);
        result->key = estimateKey (*mixFeatures);
    }
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
    applyLoadResult (std::move (result));
    applyLyricsResult();
    applyStemsResult();
}

void ChopLabProcessor::applyLoadResult (std::unique_ptr<LoadResult> result)
{
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
    d.stems = result->stems;
    d.source = result->source;
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
        d.lyrics = s.lyrics;
        d.pattern = s.pattern;
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
        d.lyrics.model = document.lyrics.model; // keep the lyrics preferences, not the words
        d.lyrics.requestedLanguage = document.lyrics.requestedLanguage;
        d.pattern.snapBeats = document.pattern.snapBeats;
        applyMarkers (d, markersFor (d));
    }

    {
        const juce::ScopedLock sl (docLock);
        document = std::move (d);
    }
    if (result->switchingSource)
    {
        selectedSlice = juce::jmin (selectedSlice, (int) document.slices.size() - 1);
    }
    else
    {
        clearHistory(); // positions in old snapshots don't apply to new (or reversed) audio
        selectedSlice = document.slices.empty() ? -1 : 0;
    }
    lastError = result->note;
    requestRender();
    publishPattern();
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
            added.settings.lyrics = {};
            added.settings.lyricsEdited = false;
            s.end = position;
            s.info = {};
            slices.insert (slices.begin() + (std::ptrdiff_t) i + 1, added);
            describeSlices (document);
            remapPatternForSplit (document.pattern, (int) i);
        }
        selectedSlice = (int) i + 1;
        publishPattern();
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
        remapPatternForMerge (document.pattern, index);
    }
    if (selectedSlice >= index)
        selectedSlice = juce::jmax (0, selectedSlice - 1);
    publishPattern();
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

    auto reversedMix = std::make_shared<SampleData> (*document.mix());
    reversedMix->audio.reverse (0, reversedMix->audio.getNumSamples());

    // Mirror everything: chops keep their settings, bar lines stay bar lines, stems flip too.
    auto job = std::make_unique<LoadJob>();
    job->sample = reversedMix;
    job->restore = true;
    if (document.stems != nullptr)
    {
        auto stems = std::make_shared<StemSet> (*document.stems);
        stems->mix = reversedMix;
        for (auto& stem : stems->stems)
        {
            auto flipped = std::make_shared<SampleData> (*stem);
            flipped->audio.reverse (0, flipped->audio.getNumSamples());
            stem = flipped;
        }
        job->stems = stems;
        job->source = document.source;
    }
    auto& s = job->saved;
    s.bpm = document.bpm;
    s.timeSig = document.timeSig;
    s.meterIsAuto = document.meterIsAuto;
    s.reversed = shouldBeReversed;
    s.global = document.global;
    s.chop = document.chop;
    // Reversed speech has no words; keep the preferences so Find lyrics is one click away.
    s.lyrics.model = document.lyrics.model;
    s.lyrics.requestedLanguage = document.lyrics.requestedLanguage;
    // Chop n becomes chop (count - 1 - n), so the pattern keeps playing the same audio.
    s.pattern = document.pattern;
    for (auto& n : s.pattern.notes)
        n.chop = (int) document.slices.size() - 1 - n.chop;

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
    return { document.slices, document.bpm,    document.downbeatSeconds, document.timeSig, document.meterIsAuto,
             document.chop,   document.global, document.pattern,         selectedSlice };
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
        document.pattern = s.pattern;
    }
    selectedSlice = juce::jlimit (-1, (int) document.slices.size() - 1, s.selected);
    lastEditKind = 0;
    publishPattern();
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
ChopLabProcessor::LyricsStatus ChopLabProcessor::getLyricsStatus() const
{
    LyricsStatus s;
    s.phase = (LyricsStatus::Phase) lyricsPhase.load();
    s.progress = lyricsProgress.load();
    {
        const juce::ScopedLock sl (resultLock);
        s.message = lyricsMessage;
    }
    if (s.phase == LyricsStatus::idle && document.lyrics.searched)
        s.phase = LyricsStatus::done;
    return s;
}

void ChopLabProcessor::findLyrics()
{
    if (! document.hasSample() || analysing)
        return;

    const int generation = ++lyricsGeneration;
    if (! lyricsSupportedOnThisCpu())
    {
        const juce::ScopedLock sl (resultLock);
        lyricsMessage = "Lyrics need a CPU with AVX2 (Intel 2013+ / AMD 2015+)";
        lyricsPhase = LyricsStatus::failed;
        sendChangeMessage();
        return;
    }

    // The separated vocal transcribes far better than the full mix.
    const auto sample = document.stems != nullptr ? document.stems->stems[stemVocals] : document.mix();
    const auto* mix = document.mix().get();
    const int model = document.lyrics.model;
    const auto language = document.lyrics.requestedLanguage;
    lyricsProgress = 0.0f;
    lyricsPhase = lyricsModelFile (model).existsAsFile() ? LyricsStatus::transcribing : LyricsStatus::downloading;
    {
        const juce::ScopedLock sl (resultLock);
        lyricsMessage = {};
    }
    sendChangeMessage();

    lyricsPool.removeAllJobs (false, 0);
    lyricsPool.addJob ([this, generation, sample, mix, model, language]
    {
        auto stillWanted = [this, generation] (float f)
        {
            lyricsProgress = f;
            return lyricsGeneration.load() == generation;
        };

        auto out = std::make_unique<LyricsJobResult>();
        out->generation = generation;
        out->sample = mix;

        bool ready = true;
        if (! lyricsModelFile (model).existsAsFile())
        {
            lyricsPhase = LyricsStatus::downloading;
            ready = downloadLyricsModel (model, stillWanted, out->result.error);
            out->result.cancelled = lyricsGeneration.load() != generation;
        }
        if (ready)
        {
            lyricsPhase = LyricsStatus::transcribing;
            lyricsProgress = 0.0f;
            out->result = transcribeLyrics (sample->audio, sample->sampleRate, model, language, stillWanted);
        }

        {
            const juce::ScopedLock sl (resultLock);
            pendingLyrics = std::move (out);
        }
        triggerAsyncUpdate();
    });
}

void ChopLabProcessor::cancelLyrics()
{
    ++lyricsGeneration;
    lyricsPhase = LyricsStatus::idle;
    sendChangeMessage();
}

void ChopLabProcessor::applyLyricsResult()
{
    std::unique_ptr<LyricsJobResult> r;
    {
        const juce::ScopedLock sl (resultLock);
        r = std::move (pendingLyrics);
    }
    if (r == nullptr || r->generation != lyricsGeneration.load())
        return;

    if (r->result.cancelled)
    {
        lyricsPhase = LyricsStatus::idle;
    }
    else if (r->result.error.isNotEmpty())
    {
        const juce::ScopedLock sl (resultLock);
        lyricsMessage = r->result.error;
        lyricsPhase = LyricsStatus::failed;
    }
    else if (r->sample == document.mix().get())
    {
        {
            const juce::ScopedLock sl (docLock);
            document.lyrics.words = std::move (r->result.words);
            document.lyrics.language = r->result.language;
            document.lyrics.searched = true;
        }
        const juce::ScopedLock sl (resultLock);
        lyricsMessage = {};
        lyricsPhase = LyricsStatus::done;
    }
    sendChangeMessage();
}

void ChopLabProcessor::setLyricsOptions (int model, const juce::String& requestedLanguage)
{
    const juce::ScopedLock sl (docLock);
    document.lyrics.model = juce::jlimit (0, (int) lyricsModels().size() - 1, model);
    document.lyrics.requestedLanguage = requestedLanguage;
    sendChangeMessage();
}

void ChopLabProcessor::setSliceLyrics (int index, const juce::String& text)
{
    if (index < 0 || index >= (int) document.slices.size() || text == document.lyricsFor (index))
        return;
    checkpoint (EditKind::slice, index);
    {
        const juce::ScopedLock sl (docLock);
        auto& s = document.slices[(size_t) index].settings;
        s.lyrics = text.trim();
        s.lyricsEdited = true;
    }
    sendChangeMessage();
}

void ChopLabProcessor::resetSliceLyrics (int index)
{
    if (index < 0 || index >= (int) document.slices.size() || ! document.slices[(size_t) index].settings.lyricsEdited)
        return;
    checkpoint (EditKind::slice, index);
    {
        const juce::ScopedLock sl (docLock);
        auto& s = document.slices[(size_t) index].settings;
        s.lyrics = {};
        s.lyricsEdited = false;
    }
    sendChangeMessage();
}

//==============================================================================
ChopLabProcessor::StemsStatus ChopLabProcessor::getStemsStatus() const
{
    StemsStatus s;
    s.phase = (StemsStatus::Phase) stemsPhase.load();
    s.progress = stemsProgress.load();
    {
        const juce::ScopedLock sl (resultLock);
        s.message = stemsMessage;
    }
    if (s.phase == StemsStatus::idle && document.stems != nullptr)
        s.phase = StemsStatus::done;
    return s;
}

void ChopLabProcessor::separateStems()
{
    if (! document.hasSample() || analysing || document.stems != nullptr)
        return;

    const int generation = ++stemsGeneration;
    if (! stemsSupportedOnThisCpu())
    {
        const juce::ScopedLock sl (resultLock);
        stemsMessage = "Stems need a CPU with AVX2 (Intel 2013+ / AMD 2015+)";
        stemsPhase = StemsStatus::failed;
        sendChangeMessage();
        return;
    }

    const auto mix = document.mix();
    const bool reversed = document.reversed;
    const auto folder = getDragFolder().getChildFile ("Stems");
    stemsProgress = 0.0f;
    stemsPhase = stemsModelLooksComplete() ? StemsStatus::separating : StemsStatus::downloading;
    {
        const juce::ScopedLock sl (resultLock);
        stemsMessage = {};
    }
    sendChangeMessage();

    stemsPool.removeAllJobs (false, 0);
    stemsPool.addJob ([this, generation, mix, reversed, folder]
    {
        auto stillWanted = [this, generation] (float f)
        {
            stemsProgress = f;
            return stemsGeneration.load() == generation;
        };

        auto out = std::make_unique<StemsJobResult>();
        out->generation = generation;
        out->mix = mix.get();

        bool ready = true;
        if (! stemsModelLooksComplete())
        {
            stemsPhase = StemsStatus::downloading;
            ready = downloadStemsModel (stillWanted, out->error);
            out->cancelled = stemsGeneration.load() != generation;
        }
        if (ready)
        {
            stemsPhase = StemsStatus::separating;
            stemsProgress = 0.0f;
            // The model only knows music played forwards, so a reversed sample is separated the right way round.
            auto result = reversed ? choplab::separateStems (forwards (*mix, true), mix->sampleRate, stillWanted)
                                   : choplab::separateStems (mix->audio, mix->sampleRate, stillWanted);
            if (result.cancelled)
                out->cancelled = true;
            else if (result.error.isNotEmpty())
                out->error = result.error;
            else
                out->stems = makeStemSet (result, mix, reversed, folder);
        }

        {
            const juce::ScopedLock sl (resultLock);
            pendingStems = std::move (out);
        }
        triggerAsyncUpdate();
    });
}

void ChopLabProcessor::cancelStems()
{
    ++stemsGeneration;
    stemsPhase = StemsStatus::idle;
    sendChangeMessage();
}

void ChopLabProcessor::applyStemsResult()
{
    std::unique_ptr<StemsJobResult> r;
    {
        const juce::ScopedLock sl (resultLock);
        r = std::move (pendingStems);
    }
    if (r == nullptr || r->generation != stemsGeneration.load())
        return;

    if (r->cancelled)
    {
        stemsPhase = StemsStatus::idle;
    }
    else if (r->error.isNotEmpty())
    {
        const juce::ScopedLock sl (resultLock);
        stemsMessage = r->error;
        stemsPhase = StemsStatus::failed;
    }
    else if (r->mix == document.mix().get() && document.stems == nullptr && r->stems != nullptr)
    {
        {
            const juce::ScopedLock sl (docLock);
            document.stems = r->stems; // the mix keeps playing until a stem is picked
        }
        const juce::ScopedLock sl (resultLock);
        stemsMessage = {};
        stemsPhase = StemsStatus::done;
    }
    sendChangeMessage();
}

void ChopLabProcessor::setSource (int source)
{
    source = juce::jlimit (-1, kNumStems - 1, source);
    if (document.stems == nullptr || analysing || source == document.source)
        return;

    // Same chops, tempo and lyrics; only the audio behind them changes.
    auto job = std::make_unique<LoadJob>();
    job->sample = document.stems->mix;
    job->stems = document.stems;
    job->source = source;
    job->switchingSource = true;
    job->restore = true;
    job->saved = document;
    job->tempo = document.detectedTempo;
    job->grid = document.detectedGrid;
    job->key = document.key;
    startLoad (std::move (job));
}

void ChopLabProcessor::useSeparatedStems (StemsResult result)
{
    if (! document.hasSample() || document.stems != nullptr || analysing)
        return;
    ++stemsGeneration;
    auto stems = makeStemSet (result, document.mix(), document.reversed, getDragFolder().getChildFile ("Stems"));
    {
        const juce::ScopedLock sl (docLock);
        document.stems = stems;
    }
    stemsPhase = StemsStatus::idle;
    sendChangeMessage();
}

juce::File ChopLabProcessor::stemFile (int stem) const
{
    if (document.stems == nullptr || stem < 0 || stem >= kNumStems)
        return {};
    const auto& stems = *document.stems;
    if (stems.files[(size_t) stem].existsAsFile())
        return stems.files[(size_t) stem];

    // Deleted since, or couldn't be saved at the time: write it again.
    auto file = stems.files[(size_t) stem];
    if (file == juce::File())
        file = getDragFolder().getChildFile ("Stems").getChildFile (legalName (stems.mix->name) + " - " + stemName (stem) + ".wav");
    file.getParentDirectory().createDirectory();
    return writeWav (forwards (*stems.stems[(size_t) stem], document.reversed), stems.mix->sampleRate, file, 32) ? file : juce::File();
}

//==============================================================================
void ChopLabProcessor::setPattern (const Pattern& p, bool newEdit)
{
    if (newEdit)
        lastEditKind = 0;
    checkpoint (EditKind::pattern);
    {
        const juce::ScopedLock sl (docLock);
        document.pattern = p;
        document.pattern.lengthBeats = juce::jlimit (1.0, 4096.0, p.lengthBeats);
    }
    publishPattern();
    sendChangeMessage();
}

void ChopLabProcessor::fillPatternFromSample()
{
    auto p = patternFromSampleOrder (document);
    p.snapBeats = document.pattern.snapBeats;
    setPattern (p, true);
}

void ChopLabProcessor::playPattern (bool shouldPlay)
{
    patternPlaying = shouldPlay;
    if (! shouldPlay)
        patternPosition = 0.0;
    sendChangeMessage();
}

void ChopLabProcessor::publishPattern()
{
    PatternData::Ptr data = new PatternData();
    data->notes = document.pattern.notes;
    data->lengthBeats = document.pattern.lengthBeats;

    PatternData::Ptr old;
    {
        const juce::SpinLock::ScopedLockType sl (patternLock);
        old = publishedPattern;
        publishedPattern = data;
    }
    if (old != nullptr)
    {
        const juce::ScopedLock sl (retiredLock);
        retiredPatterns.add (old);
    }
}

juce::AudioBuffer<float> ChopLabProcessor::renderPatternAudio (double& sampleRate)
{
    // The chops may still be rendering after an edit; the bounce should have the latest.
    for (int waited = 0; renderThread.isBusy() && waited < 15000; waited += 10)
        juce::Thread::sleep (10);
    PlaybackData::Ptr data;
    {
        const juce::SpinLock::ScopedLockType sl (publishLock);
        data = published;
    }
    const auto& pattern = document.pattern;
    if (data == nullptr || data->slices.empty() || pattern.notes.empty() || pattern.lengthBeats <= 0.0)
        return {};

    const double rate = data->sampleRate;
    sampleRate = rate;
    const double samplesPerBeat = 60.0 / juce::jmax (1.0, hostBpm.load()) * rate;
    const int loop = juce::jmax (1, (int) std::llround (pattern.lengthBeats * samplesPerBeat));

    // Play it enough times over that sounds from earlier loops have died away, then keep the last
    // loop: that's what you hear once the pattern is going round.
    int longest = 0;
    for (const auto& rs : data->slices)
        longest = juce::jmax (longest, rs.length + (int) (rs.releaseMs * 0.001f * (float) rate));
    const int loops = 1 + juce::jlimit (1, 16, 1 + longest / loop);

    struct Event
    {
        int at, chop;
        float velocity;
        double offset;
        bool on;
    };
    std::vector<Event> events;
    const int numChops = (int) data->slices.size();
    for (int k = 0; k < loops; ++k)
        for (const auto& n : pattern.notes)
        {
            if (n.chop < 0 || n.chop >= numChops || n.start >= pattern.lengthBeats)
                continue;
            const int base = k * loop;
            events.push_back ({ base + (int) (n.start * samplesPerBeat), n.chop, n.velocity, n.offset, true });
            events.push_back ({ base + juce::jmin (loop, (int) (juce::jmin (n.end(), pattern.lengthBeats) * samplesPerBeat)), n.chop, 0.0f, 0.0, false });
        }
    // Note-offs before note-ons at the same moment, as when it plays live.
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); });

    VoiceBank player;
    juce::AudioBuffer<float> all (2, loop * loops);
    all.clear();
    int pos = 0;
    for (const auto& e : events)
    {
        if (e.at > pos)
        {
            player.render (all, pos, e.at - pos, rate);
            pos = e.at;
        }
        if (e.on)
            player.start (data, e.chop, -1, e.velocity, false, true, e.offset, rate);
        else
            for (auto& v : player.voices)
                if (v.active && v.fromPattern && ! v.oneShot && ! v.releasing && v.sliceIndex == e.chop)
                    VoiceBank::release (v, false, rate);
    }
    if (pos < all.getNumSamples())
        player.render (all, pos, all.getNumSamples() - pos, rate);

    juce::AudioBuffer<float> out (2, loop);
    for (int c = 0; c < 2; ++c)
        out.copyFrom (c, 0, all, c, (loops - 1) * loop, loop);
    return out;
}

juce::File ChopLabProcessor::writePatternAudio (const juce::File& folder)
{
    double rate = 0.0;
    const auto audio = renderPatternAudio (rate);
    if (audio.getNumSamples() == 0)
        return {};
    // Named after what's in it, so an earlier bounce FL still has open is never overwritten.
    const double bpm = hostBpm.load();
    const auto tempo = std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String ((int) std::round (bpm)) : juce::String (bpm, 2);
    const auto id = juce::String::toHexString (audioHash (audio)).getLastCharacters (6);
    const auto file = folder.getChildFile (legalName (document.mix()->name) + " - pattern " + tempo + " BPM " + id + ".wav");
    if (file.existsAsFile())
        return file;
    return writeWav (audio, rate, file, 32) ? file : juce::File();
}

juce::File ChopLabProcessor::writePatternMidi (const juce::File& folder) const
{
    if (! document.hasSample() || document.pattern.notes.empty())
        return {};
    const auto midi = patternToMidi (document.pattern, document.global.rootNote, getHostBpm(), document.timeSig, (int) document.slices.size());
    const auto file = folder.getChildFile (legalName (document.sample->name) + " - piano roll.mid");
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk() || ! midi.writeTo (out))
        return {};
    return file;
}

//==============================================================================
void ChopLabProcessor::previewSlice (int index, double startSeconds)
{
    int start1, size1, start2, size2;
    previewFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
        previewQueue[(size_t) start1] = { index, startSeconds };
    previewFifo.finishedWrite (size1);
}

Processing ChopLabProcessor::chopProcessing (int index) const
{
    const auto* settings = index >= 0 && index < (int) document.slices.size() ? &document.slices[(size_t) index].settings : nullptr;
    return combine (document.global, settings, hostTempoRatio());
}

double ChopLabProcessor::chopPlaySeconds (int index) const
{
    if (index < 0 || index >= (int) document.slices.size())
        return 0.0;
    const auto& s = document.slices[(size_t) index];
    return document.secondsAt (s.end - s.start) / juce::jmax (0.01, chopProcessing (index).speed);
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
    r.stems = document.stems;
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
    for (int i = retiredPatterns.size(); --i >= 0;)
        if (retiredPatterns.getObjectPointerUnchecked (i)->getReferenceCount() == 1)
            retiredPatterns.remove (i);
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
                      .getChildFile (document.hasSample() ? legalName (document.mix()->name) : juce::String ("Untitled"));
    folder.createDirectory();
    return folder;
}

juce::AudioBuffer<float> ChopLabProcessor::renderForExport (int index) const
{
    if (! document.hasSample() || index >= (int) document.slices.size())
        return {};
    const Slice* slice = index >= 0 ? &document.slices[(size_t) index] : nullptr;
    const auto p = combine (document.global, slice != nullptr ? &slice->settings : nullptr, hostTempoRatio());
    const auto source = slice != nullptr ? document.audioFor (*slice) : document.sample;
    auto buffer = renderSegment (*source, slice != nullptr ? slice->start : 0, slice != nullptr ? slice->end : document.length(), p,
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
    const auto midi = patternToMidi (patternFromSampleOrder (document), document.global.rootNote, document.bpm, document.timeSig,
                                     (int) document.slices.size());
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
    // Always the mix: stems are files next to the project, not part of it.
    const auto mix = document.mix();
    if (encodedFor == mix.get() && encodedCache != nullptr)
        return encodedCache;

    encodedFor = mix.get();
    encodedCache = nullptr;
    if (! document.hasSample() || mix->lengthSeconds() > kMaxEmbedSeconds)
        return nullptr;

    auto encoded = std::make_shared<EncodedAudio>();
    const auto& audio = mix->audio;
    const float peak = audio.getMagnitude (0, audio.getNumSamples());
    encoded->scale = peak > 1.0f ? 1.0f / peak : 1.0f;

    juce::AudioBuffer<float> scaled (audio);
    scaled.applyGain (encoded->scale);

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (encoded->flac, false);
    juce::FlacAudioFormat flac;
    {
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                        .withSampleRate (mix->sampleRate)
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
        t.setProperty ("file", d.mix()->file.getFullPathName(), nullptr);
        t.setProperty ("name", d.mix()->name, nullptr);
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
        if (s.settings.lyricsEdited)
            st.setProperty ("lyrics", s.settings.lyrics, nullptr);
        if (s.settings.stem >= 0)
            st.setProperty ("stem", s.settings.stem, nullptr);
        slices.appendChild (st, nullptr);
    }
    t.appendChild (slices, nullptr);

    juce::ValueTree lyrics ("Lyrics");
    lyrics.setProperty ("language", d.lyrics.language, nullptr);
    lyrics.setProperty ("requested", d.lyrics.requestedLanguage, nullptr);
    lyrics.setProperty ("model", d.lyrics.model, nullptr);
    lyrics.setProperty ("searched", d.lyrics.searched, nullptr);
    for (const auto& w : d.lyrics.words)
    {
        juce::ValueTree wt ("W");
        wt.setProperty ("t", w.text, nullptr);
        wt.setProperty ("s", w.start, nullptr);
        wt.setProperty ("e", w.end, nullptr);
        lyrics.appendChild (wt, nullptr);
    }
    t.appendChild (lyrics, nullptr);

    juce::ValueTree pattern ("Pattern");
    pattern.setProperty ("length", d.pattern.lengthBeats, nullptr);
    pattern.setProperty ("snap", d.pattern.snapBeats, nullptr);
    for (const auto& n : d.pattern.notes)
    {
        juce::ValueTree nt ("N");
        nt.setProperty ("c", n.chop, nullptr);
        nt.setProperty ("s", n.start, nullptr);
        nt.setProperty ("l", n.length, nullptr);
        nt.setProperty ("v", n.velocity, nullptr);
        if (n.offset > 0.0)
            nt.setProperty ("o", n.offset, nullptr);
        pattern.appendChild (nt, nullptr);
    }
    t.appendChild (pattern, nullptr);

    if (d.stems != nullptr)
    {
        juce::ValueTree stems ("Stems");
        stems.setProperty ("source", d.source, nullptr);
        for (int i = 0; i < kNumStems; ++i)
        {
            juce::ValueTree st ("S");
            st.setProperty ("kind", i, nullptr);
            st.setProperty ("file", d.stems->files[(size_t) i].getFullPathName(), nullptr);
            st.setProperty ("hash", d.stems->hashes[(size_t) i], nullptr);
            stems.appendChild (st, nullptr);
        }
        t.appendChild (stems, nullptr);
    }
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
        s.settings.lyricsEdited = st.hasProperty ("lyrics");
        s.settings.lyrics = st.getProperty ("lyrics", "").toString();
        s.settings.stem = juce::jlimit (-1, kNumStems - 1, (int) st.getProperty ("stem", -1));
        d.slices.push_back (s);
    }

    d.lyrics = {};
    if (auto l = t.getChildWithName ("Lyrics"); l.isValid())
    {
        d.lyrics.language = l.getProperty ("language", "").toString();
        d.lyrics.requestedLanguage = l.getProperty ("requested", "").toString();
        d.lyrics.model = juce::jlimit (0, (int) lyricsModels().size() - 1, (int) l.getProperty ("model", 0));
        d.lyrics.searched = l.getProperty ("searched", false);
        for (const auto& wt : l)
            d.lyrics.words.push_back ({ wt.getProperty ("t", "").toString(), wt.getProperty ("s", 0.0), wt.getProperty ("e", 0.0), 1.0f });
    }

    d.pattern = {};
    if (auto p = t.getChildWithName ("Pattern"); p.isValid())
    {
        d.pattern.lengthBeats = juce::jlimit (1.0, 4096.0, (double) p.getProperty ("length", 16.0));
        d.pattern.snapBeats = p.getProperty ("snap", 0.25);
        for (const auto& nt : p)
            d.pattern.notes.push_back ({ nt.getProperty ("c", 0), nt.getProperty ("s", 0.0), juce::jmax (1.0 / 64.0, (double) nt.getProperty ("l", 1.0)),
                                         juce::jlimit (0.0f, 1.0f, (float) nt.getProperty ("v", 0.8f)),
                                         juce::jlimit (0.0, 60.0, (double) nt.getProperty ("o", 0.0)) });
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
        document.lyrics.model = job->saved.lyrics.model;
        document.lyrics.requestedLanguage = job->saved.lyrics.requestedLanguage;
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
    if (auto stems = t.getChildWithName ("Stems"); stems.isValid() && job->restore)
    {
        job->source = stems.getProperty ("source", -1);
        for (const auto& st : stems)
        {
            const int kind = st.getProperty ("kind", -1);
            const auto file = st.getProperty ("file", "").toString();
            if (kind >= 0 && kind < kNumStems && juce::File::isAbsolutePath (file))
            {
                job->stemFiles[(size_t) kind] = juce::File (file);
                job->stemHashes[(size_t) kind] = (juce::int64) st.getProperty ("hash", 0);
            }
        }
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
