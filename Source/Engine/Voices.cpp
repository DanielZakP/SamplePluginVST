#include "Voices.h"

namespace choplab
{

void VoiceBank::start (const PlaybackData::Ptr& audioData, int sliceIndex, int note, float velocity, bool preview, bool fromPattern,
                       double startSeconds, double currentRate)
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
            release (v, true, currentRate);

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
    v.fromPattern = fromPattern;
    v.oneShot = preview || audioData->oneShot;
    v.pos = juce::jlimit (0.0, (double) juce::jmax (0, rs->length - 2), startSeconds * audioData->sampleRate);
    v.startPos = v.pos;
    v.inc = audioData->sampleRate / currentRate;
    v.gain = rs->gain * audioData->masterGain * (preview ? 1.0f : velocity);
    const float attack = rs->attackMs * 0.001f * (float) currentRate;
    v.env = attack > 1.0f ? 0.0f : 1.0f;
    v.attackStep = attack > 1.0f ? 1.0f / attack : 1.0f;
    v.releaseStep = 1.0f / juce::jmax (1.0f, rs->releaseMs * 0.001f * (float) currentRate);
    v.releasing = false;
    v.active = true;
    v.age = ++counter;
}

void VoiceBank::release (Voice& v, bool fast, double currentRate)
{
    v.releasing = true;
    if (fast)
        v.releaseStep = juce::jmax (v.releaseStep, 1.0f / (0.004f * (float) currentRate));
}

void VoiceBank::render (juce::AudioBuffer<float>& buffer, int start, int num, double currentRate)
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
        // Short fades at the chop edges so cutting mid-waveform never clicks (a little longer when a
        // note starts partway into the chop, which is usually mid-waveform)
        const double fadeIn = (v.startPos > 0.0 ? 0.002 : 0.0003) * currentRate / v.inc, fadeOut = 0.003 * currentRate / v.inc;
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
            const float edge = (float) juce::jmin (1.0, (v.pos - v.startPos) / fadeIn, remaining / fadeOut);
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

void VoiceBank::reset()
{
    for (auto& v : voices)
    {
        v.active = false;
        v.data = nullptr;
        v.slice = nullptr;
    }
}

bool VoiceBank::anyActive() const
{
    for (const auto& v : voices)
        if (v.active)
            return true;
    return false;
}

} // namespace choplab
