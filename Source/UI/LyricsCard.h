#pragma once

#include "../PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab
{

// The "Lyrics" card: language and model choice, the Find lyrics button and progress.
class LyricsCard : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit LyricsCard (ChopLabProcessor&);

    void documentChanged();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::String statusText (juce::Colour& colour) const;

    ChopLabProcessor& proc;
    juce::TextButton findButton { "Find lyrics" };
    juce::ComboBox language, model;
    std::vector<juce::String> codes; // language combo id - 2 -> code
    juce::Rectangle<int> captionArea, statusArea;
    int lastPhase = -1;
};

} // namespace choplab
