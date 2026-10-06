#pragma once

#include "Renderer.h"
#include <array>

namespace choplab
{

// One chop playing.
struct Voice
{
    PlaybackData::Ptr data;
    const RenderedSlice* slice = nullptr;
    int sliceIndex = -1;
    int note = -1;
    bool preview = false;
    bool oneShot = false;
    double pos = 0.0, inc = 1.0;
    float gain = 1.0f, env = 0.0f, attackStep = 1.0f, releaseStep = 0.0f;
    bool releasing = false;
    bool active = false;
    bool fromPattern = false;
    double startPos = 0.0; // where it started in the chop (later than 0 for notes with an offset)
    juce::uint32 age = 0;
};

// The chop player. The plugin runs one on the audio thread; bouncing the pattern to audio runs
// another offline, so the bounce sounds exactly like what plays.
class VoiceBank
{
public:
    std::array<Voice, 32> voices;

    // sliceIndex -1 plays the whole sample. startSeconds: how far into the chop to start.
    void start (const PlaybackData::Ptr&, int sliceIndex, int note, float velocity, bool preview, bool fromPattern, double startSeconds,
                double outputRate);
    static void release (Voice&, bool fast, double outputRate);
    void render (juce::AudioBuffer<float>&, int start, int num, double outputRate);
    void reset();
    bool anyActive() const;

private:
    juce::uint32 counter = 0;
};

} // namespace choplab
