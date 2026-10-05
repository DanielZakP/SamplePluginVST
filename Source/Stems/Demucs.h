#pragma once

// The only file that talks to demucs.cpp. It's built in its own library with AVX2 switched on, so
// it deliberately includes nothing from JUCE: keep JUCE code out of AVX2-compiled files, or a
// non-AVX2 CPU could end up running AVX2 copies of JUCE functions.

#include <functional>
#include <string>
#include <vector>

namespace choplab::demucs
{

constexpr int kSampleRate = 44100;
constexpr int kNumSources = 4; // the model's order: drums, bass, other, vocals

// Splits stereo audio at 44.1 kHz into the model's four sources. out is resized to
// kNumSources * 2 * numSamples: source s, channel c starts at (s * 2 + c) * numSamples.
// Runs the model's 7.8-second segments on `workers` threads, calling onWorkerStart (if set) at the
// start of each. progress is called on the calling thread with 0..1 and returns false to cancel.
// Returns an empty string on success, "cancelled", or what went wrong.
std::string separate (const std::string& modelPath, const float* left, const float* right, int numSamples, int workers,
                      const std::function<bool (float)>& progress, const std::function<void()>& onWorkerStart,
                      std::vector<float>& out);

// How many segments separate() runs for this many samples (for working out how many threads are worth it).
int segmentCount (int numSamples);

} // namespace choplab::demucs
