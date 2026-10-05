#include "Chopper.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace choplab
{

std::vector<juce::int64> transientMarkers (const std::vector<OnsetCandidate>& onsets, float sensitivity, float minLengthMs,
                                           double sampleRate, juce::int64 length)
{
    // Higher sensitivity lets weaker hits through. Strongest hits are placed first so a weak
    // ghost note can never push out the real hit next to it.
    const float threshold = 0.6f * std::pow (1.0f - juce::jlimit (0.0f, 1.0f, sensitivity), 2.0f);
    const auto minGap = (juce::int64) (minLengthMs * 0.001 * sampleRate);

    std::vector<OnsetCandidate> sorted;
    for (const auto& o : onsets)
        if (o.strength >= threshold && o.position < length)
            sorted.push_back (o);
    std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.strength > b.strength; });

    std::vector<juce::int64> markers { 0 };
    for (const auto& o : sorted)
    {
        if ((int) markers.size() >= kMaxSlices)
            break;
        const bool tooClose = std::any_of (markers.begin(), markers.end(),
                                           [&] (juce::int64 m) { return std::abs (m - o.position) < minGap; });
        const bool tooCloseToEnd = length - o.position < minGap;
        if (! tooClose && ! tooCloseToEnd)
            markers.push_back (o.position);
    }

    std::sort (markers.begin(), markers.end());
    return markers;
}

std::vector<juce::int64> gridMarkers (double bpm, double downbeatSeconds, TimeSignature ts, double gridBeats, double sampleRate,
                                      juce::int64 length)
{
    const double stepBeats = gridBeats > 0.0 ? gridBeats : ts.quarterBeatsPerBar();
    const double stepSeconds = stepBeats * 60.0 / juce::jmax (1.0, bpm);
    std::vector<juce::int64> markers { 0 };
    if (stepSeconds <= 0.0)
        return markers;

    // Grid lines are anchored to bar 1, so chops line up with bars even when the sample has a pickup.
    double t = downbeatSeconds - std::floor (downbeatSeconds / stepSeconds) * stepSeconds;
    const double minGapSeconds = 0.01;
    for (; t * sampleRate < (double) length; t += stepSeconds)
    {
        const auto pos = (juce::int64) std::llround (t * sampleRate);
        if (t > minGapSeconds && (double) (length - pos) / sampleRate > minGapSeconds && (int) markers.size() < kMaxSlices)
            markers.push_back (pos);
    }
    return markers;
}

void applyMarkers (Document& doc, std::vector<juce::int64> markers)
{
    const auto length = doc.length();
    std::sort (markers.begin(), markers.end());
    markers.erase (std::unique (markers.begin(), markers.end()), markers.end());
    markers.erase (std::remove_if (markers.begin(), markers.end(), [&] (juce::int64 m) { return m < 0 || m >= length; }), markers.end());
    if (markers.empty() || markers.front() != 0)
        markers.insert (markers.begin(), 0);
    if ((int) markers.size() > kMaxSlices)
        markers.resize ((size_t) kMaxSlices);

    std::map<juce::int64, Slice> previous;
    for (const auto& s : doc.slices)
        previous[s.start] = s;

    std::vector<Slice> slices;
    for (size_t i = 0; i < markers.size(); ++i)
    {
        Slice s;
        s.start = markers[i];
        s.end = i + 1 < markers.size() ? markers[i + 1] : length;
        if (auto it = previous.find (s.start); it != previous.end())
        {
            s.settings = it->second.settings;
            if (it->second.end == s.end)
                s.info = it->second.info;
        }
        slices.push_back (s);
    }
    doc.slices = std::move (slices);
    describeSlices (doc);
}

void describeSlices (Document& doc)
{
    if (! doc.hasSample() || doc.features == nullptr)
        return;
    for (auto& s : doc.slices)
        if (s.info.type.isEmpty())
            s.info = describeSlice (*doc.features, doc.sample->audio, s.start, s.end);
}

} // namespace choplab
