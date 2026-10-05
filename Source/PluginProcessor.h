#pragma once

#include "Engine/Chopper.h"
#include "Engine/Model.h"
#include "Engine/Pattern.h"
#include "Engine/Renderer.h"
#include "Lyrics/Lyrics.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>

class ChopLabProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    ChopLabProcessor();
    ~ChopLabProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Everything below is called from the message thread (the editor).
    const choplab::Document& doc() const { return document; }
    bool isAnalysing() const { return analysing; }
    bool isRendering() const { return renderThread.isBusy(); }
    juce::String getError() const { return lastError; }

    void loadFile (const juce::File&);
    static bool canLoad (const juce::File&);

    void setBpm (double);
    void setTimeSignature (choplab::TimeSignature, bool automatic);
    void setDownbeat (double seconds);
    void setChopSettings (const choplab::ChopSettings&, bool rechopNow);
    void rechop();
    void moveMarker (int sliceIndex, juce::int64 newStart, bool finished);
    void addMarker (juce::int64 position);
    void removeMarker (int sliceIndex);
    void setSliceSettings (int index, const choplab::SliceSettings&);
    void setGlobalSettings (const choplab::GlobalSettings&);
    void setReversed (bool);

    int selectedSlice = -1;
    void selectSlice (int index);

    // Undo/redo for everything you edit on a loaded sample (chops, labels, tempo, settings).
    void undo();
    void redo();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }

    void previewSlice (int index);
    void previewFull();
    void stopPreview();

    // Lyrics: runs in the background (downloading the model first if needed).
    struct LyricsStatus
    {
        enum Phase
        {
            idle,
            downloading,
            transcribing,
            done,
            failed
        };
        Phase phase = idle;
        float progress = 0.0f;
        juce::String message;
    };
    LyricsStatus getLyricsStatus() const;
    void findLyrics();
    void cancelLyrics();
    void setLyricsOptions (int model, const juce::String& requestedLanguage);
    void setSliceLyrics (int index, const juce::String& text);
    void resetSliceLyrics (int index);

    // Built-in piano roll
    // newEdit = start of a separate undo step; pass false while dragging so the drag is one step.
    void setPattern (const choplab::Pattern&, bool newEdit = true);
    void fillPatternFromSample();
    void playPattern (bool shouldPlay);
    bool isPatternPlaying() const { return patternPlaying.load(); }
    double getPatternPosition() const { return patternPosition.load(); }
    juce::File writePatternMidi (const juce::File& folder) const;
    int currentPage = 0; // editor page: 0 = chops, 1 = pattern

    double getHostBpm() const { return hostBpm.load(); }
    void getPlayingPositions (std::vector<juce::int64>&) const;

    juce::File getDragFolder() const;
    juce::File renderSliceToFile (int index, const juce::File& folder) const;
    juce::File renderFullToFile (const juce::File& folder) const;
    juce::File writeMidiPattern (const juce::File& folder) const;
    int exportAllSlices (const juce::File& folder) const;

    static constexpr int kChangeAnalysis = 1;

private:
    //==============================================================================
    struct LoadJob;
    struct LoadResult;
    void startLoad (std::unique_ptr<LoadJob>);
    static std::unique_ptr<LoadResult> runLoad (const LoadJob&);
    void handleAsyncUpdate() override;
    void applyLoadResult (std::unique_ptr<LoadResult>);
    void applyLyricsResult();
    void timerCallback() override;

    std::vector<juce::int64> markersFor (const choplab::Document&) const;
    void refreshGrid();
    void requestRender();
    void publish (choplab::PlaybackData::Ptr);
    void releaseRetired();
    double hostTempoRatio() const;
    juce::AudioBuffer<float> renderForExport (int index) const; // -1 = whole sample

    struct Snapshot
    {
        std::vector<choplab::Slice> slices;
        double bpm = 120.0, downbeatSeconds = 0.0;
        choplab::TimeSignature timeSig;
        bool meterIsAuto = true;
        choplab::ChopSettings chop;
        choplab::GlobalSettings global;
        choplab::Pattern pattern;
        int selected = -1;
    };
    enum class EditKind
    {
        tempo = 1, timeSig, downbeat, chop, marker, addMarker, removeMarker, global, pattern, slice = 1000
    };
    Snapshot snapshot() const;
    void restore (const Snapshot&);
    void checkpoint (EditKind, int detail = 0); // call before an edit; rapid repeats of the same edit merge
    void clearHistory();

    std::vector<Snapshot> undoStack, redoStack;
    int lastEditKind = 0;
    juce::uint32 lastEditTime = 0;

    juce::ValueTree toValueTree() const;
    static void readSettings (const juce::ValueTree&, choplab::Document&);
    struct EncodedAudio
    {
        juce::MemoryBlock flac;
        float scale = 1.0f; // gain applied before encoding so float audio above 0 dBFS survives
    };
    std::shared_ptr<const EncodedAudio> encodedAudio() const;

    choplab::Document document;
    juce::CriticalSection docLock; // guards document against state saves from other threads
    std::atomic<bool> analysing { false };
    juce::String lastError;
    std::atomic<int> loadGeneration { 0 };

    juce::ThreadPool loadPool { 1 };
    juce::CriticalSection resultLock;
    std::unique_ptr<LoadResult> pendingResult;

    struct LyricsJobResult
    {
        int generation = 0;
        const choplab::SampleData* sample = nullptr;
        choplab::LyricsResult result;
    };
    juce::ThreadPool lyricsPool { 1 };
    std::atomic<int> lyricsGeneration { 0 };
    std::atomic<int> lyricsPhase { LyricsStatus::idle };
    std::atomic<float> lyricsProgress { 0.0f };
    juce::String lyricsMessage; // guarded by resultLock
    std::unique_ptr<LyricsJobResult> pendingLyrics;

    mutable juce::CriticalSection encodedLock;
    mutable std::shared_ptr<const EncodedAudio> encodedCache;
    mutable const choplab::SampleData* encodedFor = nullptr;

    //==============================================================================
    // Audio thread
    struct Voice
    {
        choplab::PlaybackData::Ptr data;
        const choplab::RenderedSlice* slice = nullptr;
        int sliceIndex = -1;
        int note = -1;
        bool preview = false;
        bool oneShot = false;
        double pos = 0.0, inc = 1.0;
        float gain = 1.0f, env = 0.0f, attackStep = 1.0f, releaseStep = 0.0f;
        bool releasing = false;
        bool active = false;
        bool fromPattern = false;
        juce::uint32 age = 0;
    };

    void handleMidi (const juce::MidiMessage&);
    void startVoice (int sliceIndex, int note, float velocity, bool preview, bool fromPattern = false);
    void addPatternEvents (int numSamples);
    void publishPattern();
    void releaseVoice (Voice&, bool fast);
    void renderVoices (juce::AudioBuffer<float>&, int start, int num);

    std::array<Voice, 32> voices;
    juce::uint32 voiceCounter = 0;
    choplab::PlaybackData::Ptr audioData;
    double currentRate = 44100.0;

    juce::SpinLock publishLock;
    choplab::PlaybackData::Ptr published;
    juce::CriticalSection retiredLock;
    juce::ReferenceCountedArray<choplab::PlaybackData> retired;

    juce::AbstractFifo previewFifo { 32 };
    std::array<int, 32> previewQueue {};
    static constexpr int kPreviewFull = -1, kPreviewStop = -2;

    std::atomic<double> hostBpm { 120.0 };
    double renderedHostBpm = 0.0;
    std::atomic<double> preparedRate { 0.0 };
    double renderedRate = 0.0;
    std::array<std::atomic<juce::int64>, 16> playPositions;

    // Pattern playback: the message thread publishes note lists, the audio thread plays them.
    struct PatternData : juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<PatternData>;
        std::vector<choplab::PatternNote> notes;
        double lengthBeats = 16.0;
    };
    juce::SpinLock patternLock;
    PatternData::Ptr publishedPattern, audioPattern;
    juce::ReferenceCountedArray<PatternData> retiredPatterns; // guarded by retiredLock
    std::atomic<bool> patternPlaying { false };
    bool patternWasPlaying = false;
    double patternBeat = 0.0;
    std::atomic<double> patternPosition { 0.0 };
    juce::MidiBuffer combinedMidi;
    static constexpr int kPatternChannel = 16;

    choplab::RenderThread renderThread { [this] (choplab::PlaybackData::Ptr d) { publish (d); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChopLabProcessor)
};
