#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace choplab
{

// Downloads url to target through a .part file, so a cut-off download never looks finished.
// `what` names the file in error messages ("the lyrics model"). progress gets 0..1 and returns
// false to cancel. Blocking; call from a background thread.
bool downloadFile (const juce::URL& url, const juce::File& target, juce::int64 approxBytes, const juce::String& what,
                   const std::function<bool (float)>& progress, juce::String& error);

} // namespace choplab
