#pragma once

#include "Model.h"

namespace choplab
{

// Marker positions (slice starts, in source samples). The first is always 0.
std::vector<juce::int64> transientMarkers (const std::vector<OnsetCandidate>& onsets, float sensitivity, float minLengthMs,
                                           double sampleRate, juce::int64 length);
std::vector<juce::int64> gridMarkers (double bpm, double downbeatSeconds, TimeSignature, double gridBeats, double sampleRate,
                                      juce::int64 length);

// Rebuild slices from markers. Settings are kept for slices whose start didn't move.
void applyMarkers (Document&, std::vector<juce::int64> markers);
void describeSlices (Document&);

} // namespace choplab
