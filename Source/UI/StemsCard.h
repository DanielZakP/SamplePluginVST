#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab
{

// The "Stems" card: separate the sample, then pick which stem the chops play and drag stems out.
class StemsCard : public juce::Component,
                  public juce::SettableTooltipClient,
                  private juce::Timer
{
public:
    explicit StemsCard (ChopLabProcessor&);

    void documentChanged();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::String statusText (juce::Colour& colour) const;
    juce::StringArray filesToDrag() const;

    ChopLabProcessor& proc;
    juce::TextButton separateButton { "Separate stems" };
    juce::ComboBox source;
    DragOutButton drag;
    juce::Rectangle<int> captionArea, statusArea;
    int lastPhase = -1;
    double separatingSince = 0.0; // ms, for the time-left estimate
};

} // namespace choplab
