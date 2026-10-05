#pragma once

#include "Model.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <map>

namespace choplab
{

// How one chop ends up being processed once slice and global settings are combined.
struct Processing
{
    double speed = 1.0;        // overall playback rate (length is divided by this)
    double transpose = 0.0;    // semitones, including any tape-style pitch from speed
    bool tapeOnly = true;      // pitch change comes only from tape-style speed: plain resampling, best quality
    bool reverse = false;

    bool isIdentity() const { return std::abs (speed - 1.0) < 1.0e-6 && std::abs (transpose) < 1.0e-6; }
};

Processing combine (const GlobalSettings&, const SliceSettings*, double hostTempoRatio);

// Resamples every channel. ratio = input samples per output sample (2 = half as many samples).
juce::AudioBuffer<float> resampleAudio (const juce::AudioBuffer<float>&, double ratio);

// Renders [start, end) of the sample with the given processing, resampled to targetRate.
juce::AudioBuffer<float> renderSegment (const SampleData&, juce::int64 start, juce::int64 end, const Processing&, double targetRate);

// What the audio thread plays: every chop pre-rendered with its pitch/speed/reverse applied.
struct RenderedSlice
{
    // Unprocessed chops point straight into the source audio (no copy); processed ones own a buffer.
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    int offset = 0, length = 0;
    juce::int64 sourceStart = 0, sourceEnd = 0;
    bool reverse = false;
    float gain = 1.0f;
    float attackMs = 0.0f, releaseMs = 15.0f;
};

struct PlaybackData : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<PlaybackData>;

    double sampleRate = 44100.0;
    std::vector<RenderedSlice> slices;
    RenderedSlice full; // whole sample with global settings, for previews
    int rootNote = 60;
    bool oneShot = false;
    bool mono = false;
    float masterGain = 1.0f;
};

struct RenderRequest
{
    std::shared_ptr<const SampleData> sample;
    std::vector<Slice> slices;
    GlobalSettings global;
    double hostTempoRatio = 1.0;
    double targetRate = 44100.0;
};

// Renders on a background thread. A new request supersedes one in progress; chops that
// didn't change are reused, so tweaking one chop only re-renders that chop.
class RenderThread : private juce::Thread
{
public:
    using Publish = std::function<void (PlaybackData::Ptr)>;

    explicit RenderThread (Publish);
    ~RenderThread() override;

    void request (RenderRequest);
    bool isBusy() const { return busy.load(); }

private:
    void run() override;
    bool renderPass (const RenderRequest&);
    std::shared_ptr<const juce::AudioBuffer<float>> renderCached (const RenderRequest&, juce::int64 start, juce::int64 end,
                                                                  const Processing&, std::map<juce::uint64, std::shared_ptr<const juce::AudioBuffer<float>>>& used);

    Publish publish;
    juce::CriticalSection lock;
    std::unique_ptr<RenderRequest> pending;
    std::atomic<bool> busy { false };
    std::map<juce::uint64, std::shared_ptr<const juce::AudioBuffer<float>>> cache;
};

} // namespace choplab
