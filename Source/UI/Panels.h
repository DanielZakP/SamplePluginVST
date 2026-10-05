#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace choplab
{

// Settings for the selected chop.
class SliceInspector : public juce::Component
{
public:
    SliceInspector (ChopLabProcessor&, std::function<juce::File (int)> exportChop);

    void documentChanged();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void push();
    void pushLyrics();
    SliceSettings current() const;
    bool showingValidChop() const;

    ChopLabProcessor& proc;
    std::function<juce::File (int)> exportChop;
    juce::TextEditor label, lyrics;
    juce::TextButton playButton { "Play" }, barOneButton { "Set as bar 1" }, resetButton { "Reset" };
    DragOutButton dragWav;
    Knob pitch, speed, gain, attack, release;
    juce::ToggleButton keepPitch { "Keep pitch" }, reverse { "Reverse" };
    juce::Rectangle<int> headerArea, infoArea;
    int shown = -2;
};

// Settings for the whole sample and how chops are triggered.
class GlobalPanel : public juce::Component
{
public:
    explicit GlobalPanel (ChopLabProcessor&);

    void documentChanged();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void push();

    ChopLabProcessor& proc;
    Knob pitch, speed, volume;
    juce::ToggleButton keepPitch { "Keep pitch" }, reverseSample { "Reverse sample" }, sync { "Sync to project tempo" },
        mono { "Mono (new chop cuts the last)" };
    juce::ComboBox rootNote, trigger;
    DragOutButton dragFull;
    juce::Rectangle<int> captionArea, rootCaption, triggerCaption;
};

} // namespace choplab
