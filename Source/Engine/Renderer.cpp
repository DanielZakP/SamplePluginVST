#include "Renderer.h"

#include "signalsmith-stretch/signalsmith-stretch.h"
#include <cmath>
#include <cstring>

namespace choplab
{

Processing combine (const GlobalSettings& g, const SliceSettings* s, double hostTempoRatio)
{
    Processing p;
    const double globalSpeed = (double) g.speed * (g.syncToHost ? hostTempoRatio : 1.0);
    const double sliceSpeed = s != nullptr ? (double) s->speed : 1.0;
    const double tape = (g.keepPitch ? 1.0 : globalSpeed) * (s != nullptr && ! s->keepPitch ? sliceSpeed : 1.0);
    const double pitch = (double) g.pitch + (s != nullptr ? (double) s->pitch : 0.0);

    p.speed = globalSpeed * sliceSpeed;
    p.transpose = pitch + 12.0 * std::log2 (tape);
    p.tapeOnly = std::abs (pitch) < 1.0e-6 && std::abs (p.speed / tape - 1.0) < 1.0e-6;
    p.reverse = s != nullptr && s->reverse;
    return p;
}

juce::AudioBuffer<float> resampleAudio (const juce::AudioBuffer<float>& in, double ratio)
{
    // ratio = input samples consumed per output sample
    const int numOut = juce::jmax (1, (int) std::floor (in.getNumSamples() / ratio));
    juce::AudioBuffer<float> out (in.getNumChannels(), numOut);
    std::vector<float> padded ((size_t) in.getNumSamples() + 64, 0.0f);

    for (int c = 0; c < in.getNumChannels(); ++c)
    {
        std::copy (in.getReadPointer (c), in.getReadPointer (c) + in.getNumSamples(), padded.begin());
        juce::WindowedSincInterpolator interp;
        interp.process (ratio, padded.data(), out.getWritePointer (c), numOut);
    }

    // The interpolator has a short delay; trim it so transients stay where they were.
    const int latency = (int) std::round (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
    if (latency > 0 && latency < numOut)
    {
        for (int c = 0; c < out.getNumChannels(); ++c)
        {
            auto* d = out.getWritePointer (c);
            std::memmove (d, d + latency, sizeof (float) * (size_t) (numOut - latency));
            std::fill (d + numOut - latency, d + numOut, 0.0f);
        }
    }
    return out;
}

namespace
{
juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& in, double rate, double speed, double transpose)
{
    const int channels = in.getNumChannels();
    const int inLen = in.getNumSamples();
    const int wantedOut = juce::jmax (1, (int) std::llround (inLen / speed));

    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault (channels, (float) rate);
    st.setTransposeSemitones ((float) transpose);

    // Very short chops are padded with silence: the stretcher needs about one block of input.
    const int minIn = st.outputSeekLength ((float) speed) + 1;
    const int paddedIn = juce::jmax (inLen, minIn);
    const int paddedOut = juce::jmax (1, (int) std::llround (paddedIn / speed));

    juce::AudioBuffer<float> src (channels, paddedIn);
    src.clear();
    for (int c = 0; c < channels; ++c)
        src.copyFrom (c, 0, in, c, 0, inLen);

    juce::AudioBuffer<float> out (channels, paddedOut);
    out.clear();
    st.exact (src.getArrayOfReadPointers(), paddedIn, out.getArrayOfWritePointers(), paddedOut);
    out.setSize (channels, juce::jmin (wantedOut, paddedOut), true);
    return out;
}

void applyEdgeFades (juce::AudioBuffer<float>& b, double rate)
{
    const int n = b.getNumSamples();
    const int fadeIn = juce::jmin (n / 4, (int) (rate * 0.0003));
    const int fadeOut = juce::jmin (n / 2, (int) (rate * 0.003));
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        if (fadeIn > 0)
            b.applyGainRamp (c, 0, fadeIn, 0.0f, 1.0f);
        if (fadeOut > 0)
            b.applyGainRamp (c, n - fadeOut, fadeOut, 1.0f, 0.0f);
    }
}
} // namespace

juce::AudioBuffer<float> renderSegment (const SampleData& sample, juce::int64 start, juce::int64 end, const Processing& p, double targetRate)
{
    start = juce::jlimit ((juce::int64) 0, (juce::int64) sample.audio.getNumSamples(), start);
    end = juce::jlimit (start, (juce::int64) sample.audio.getNumSamples(), end);
    const int len = (int) (end - start);
    const int channels = juce::jlimit (1, 2, sample.audio.getNumChannels());

    juce::AudioBuffer<float> seg (channels, juce::jmax (1, len));
    seg.clear();
    for (int c = 0; c < channels; ++c)
        seg.copyFrom (c, 0, sample.audio, c, (int) start, len);
    if (p.reverse)
        seg.reverse (0, seg.getNumSamples());

    const double rateRatio = sample.sampleRate / targetRate;
    juce::AudioBuffer<float> out;

    if (p.isIdentity() || p.tapeOnly)
    {
        const double ratio = rateRatio * p.speed; // tape-style: speed and pitch move together
        out = std::abs (ratio - 1.0) < 1.0e-9 ? seg : resampleAudio (seg, ratio);
    }
    else
    {
        if (std::abs (rateRatio - 1.0) > 1.0e-9)
            seg = resampleAudio (seg, rateRatio);
        out = stretch (seg, targetRate, p.speed, p.transpose);
    }

    applyEdgeFades (out, targetRate);
    return out;
}

//==============================================================================
RenderThread::RenderThread (Publish p) : juce::Thread ("ChopLab render"), publish (std::move (p))
{
    startThread (juce::Thread::Priority::normal);
}

RenderThread::~RenderThread()
{
    stopThread (10000);
}

void RenderThread::request (RenderRequest r)
{
    {
        const juce::ScopedLock sl (lock);
        pending = std::make_unique<RenderRequest> (std::move (r));
    }
    busy = true;
    notify();
}

void RenderThread::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<RenderRequest> job;
        {
            const juce::ScopedLock sl (lock);
            job = std::move (pending);
        }

        if (job == nullptr)
        {
            busy = false;
            wait (-1);
            continue;
        }

        renderPass (*job);
    }
}

namespace
{
juce::uint64 hashKey (const void* sample, juce::int64 start, juce::int64 end, const Processing& p, double rate)
{
    juce::uint64 h = 1469598103934665603ull;
    auto mix = [&h] (const void* data, size_t size)
    {
        const auto* bytes = static_cast<const juce::uint8*> (data);
        for (size_t i = 0; i < size; ++i)
            h = (h ^ bytes[i]) * 1099511628211ull;
    };
    const auto ptr = (juce::uint64) (juce::pointer_sized_uint) sample;
    mix (&ptr, sizeof (ptr));
    mix (&start, sizeof (start));
    mix (&end, sizeof (end));
    mix (&p.speed, sizeof (p.speed));
    mix (&p.transpose, sizeof (p.transpose));
    const int flags = (p.reverse ? 1 : 0) | (p.tapeOnly ? 2 : 0);
    mix (&flags, sizeof (flags));
    mix (&rate, sizeof (rate));
    return h;
}
} // namespace

std::shared_ptr<const juce::AudioBuffer<float>> RenderThread::renderCached (const std::shared_ptr<const SampleData>& sample, double targetRate,
                                                                            juce::int64 start, juce::int64 end, const Processing& p,
                                                                            std::map<juce::uint64, std::shared_ptr<const juce::AudioBuffer<float>>>& used)
{
    const auto key = hashKey (sample.get(), start, end, p, targetRate);
    auto it = cache.find (key);
    if (it == cache.end())
        it = cache.emplace (key, std::make_shared<const juce::AudioBuffer<float>> (renderSegment (*sample, start, end, p, targetRate))).first;
    used[key] = it->second;
    return it->second;
}

namespace
{
bool playsUnprocessed (const Processing& p, const SampleData& sample, double targetRate)
{
    return p.isIdentity() && ! p.reverse && std::abs (sample.sampleRate - targetRate) < 0.5;
}

std::shared_ptr<const juce::AudioBuffer<float>> sourceView (const std::shared_ptr<const SampleData>& sample)
{
    return { sample, &sample->audio }; // shares ownership of the sample, no copy
}
} // namespace

bool RenderThread::renderPass (const RenderRequest& r)
{
    PlaybackData::Ptr data = new PlaybackData();
    data->sampleRate = r.targetRate;
    data->rootNote = r.global.rootNote;
    data->oneShot = r.global.oneShot;
    data->mono = r.global.mono;
    data->masterGain = juce::Decibels::decibelsToGain (r.global.gainDb);

    std::map<juce::uint64, std::shared_ptr<const juce::AudioBuffer<float>>> used;

    auto superseded = [this]
    {
        const juce::ScopedLock sl (lock);
        return pending != nullptr || threadShouldExit();
    };

    if (r.sample != nullptr)
    {
        for (const auto& s : r.slices)
        {
            if (superseded())
                return false; // keep what we rendered in the cache and start on the newer request

            const auto p = combine (r.global, &s.settings, r.hostTempoRatio);
            const bool ownStem = r.stems != nullptr && s.settings.stem >= 0 && s.settings.stem < kNumStems;
            const auto& source = ownStem ? r.stems->stems[(size_t) s.settings.stem] : r.sample;
            RenderedSlice rs;
            if (playsUnprocessed (p, *source, r.targetRate))
            {
                rs.audio = sourceView (source);
                rs.offset = (int) s.start;
                rs.length = (int) (s.end - s.start);
            }
            else
            {
                rs.audio = renderCached (source, r.targetRate, s.start, s.end, p, used);
                rs.length = rs.audio->getNumSamples();
            }
            rs.sourceStart = s.start;
            rs.sourceEnd = s.end;
            rs.reverse = s.settings.reverse;
            rs.gain = juce::Decibels::decibelsToGain (s.settings.gainDb);
            rs.attackMs = s.settings.attackMs;
            rs.releaseMs = s.settings.releaseMs;
            data->slices.push_back (rs);
        }

        if (superseded())
            return false;

        // The whole-sample render can take a moment on a long file, so let the chops play first.
        const auto p = combine (r.global, nullptr, r.hostTempoRatio);
        const auto length = (juce::int64) r.sample->audio.getNumSamples();
        data->full.sourceEnd = length;
        if (playsUnprocessed (p, *r.sample, r.targetRate))
        {
            data->full.audio = sourceView (r.sample);
        }
        else
        {
            if (cache.find (hashKey (r.sample.get(), 0, length, p, r.targetRate)) == cache.end())
            {
                PlaybackData::Ptr early = new PlaybackData (*data);
                publish (early);
            }
            data->full.audio = renderCached (r.sample, r.targetRate, 0, length, p, used);
        }
        data->full.length = data->full.audio->getNumSamples();
    }

    cache = std::move (used);
    publish (data);
    return true;
}

} // namespace choplab
