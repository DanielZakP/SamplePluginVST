#pragma once

#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab
{

// Rotary knob with a caption above and a typeable value below. Double-click resets.
class Knob : public juce::Component
{
public:
    Knob (const juce::String& name, double min, double max, double defaultValue, double interval,
          std::function<juce::String (double)> format, double skewCentre = 0.0)
        : caption ({}, name)
    {
        caption.setFont (theme::font (12.0f));
        caption.setColour (juce::Label::textColourId, theme::textDim);
        caption.setJustificationType (juce::Justification::centred);
        caption.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (caption);

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
        slider.setRange (min, max, interval);
        if (skewCentre > min && skewCentre < max)
            slider.setSkewFactorFromMidPoint (skewCentre);
        slider.setDoubleClickReturnValue (true, defaultValue);
        slider.setValue (defaultValue, juce::dontSendNotification);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.textFromValueFunction = format;
        slider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-0123456789.").getDoubleValue(); };
        slider.onValueChange = [this]
        {
            if (onChange)
                onChange (slider.getValue());
        };
        addAndMakeVisible (slider);
        slider.updateText();
    }

    void setValue (double v) { slider.setValue (v, juce::dontSendNotification); }
    double getValue() const { return slider.getValue(); }

    void resized() override
    {
        auto r = getLocalBounds();
        caption.setBounds (r.removeFromTop (14));
        slider.setBounds (r);
    }

    std::function<void (double)> onChange;
    juce::Slider slider;

private:
    juce::Label caption;
};

// Drag this out of the plugin to drop a freshly rendered file into FL Studio (playlist, channel rack, piano roll).
class DragOutButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    DragOutButton (const juce::String& t, std::function<juce::File()> make) : text (t), makeFile (std::move (make))
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void setText (const juce::String& t)
    {
        if (t != text)
        {
            text = t;
            repaint();
        }
    }

    // Set to drag several files at once instead of the one from makeFile.
    std::function<juce::StringArray()> makeFiles;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        const auto c = theme::panelRaised.brighter (hover ? 0.06f : 0.0f).withMultipliedAlpha (alpha);
        g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.05f), r.getY(), c.darker (0.1f), r.getBottom()));
        g.fillRoundedRectangle (r, theme::radius);
        g.setColour (theme::edge.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r.reduced (0.5f), theme::radius, 1.0f);

        // Grip: you pick this up and drop it somewhere
        g.setColour (theme::textDim.withMultipliedAlpha (alpha));
        for (int i = 0; i < 3; ++i)
            g.fillRect (8.0f, r.getCentreY() - 4.0f + (float) i * 3.0f, 8.0f, 1.0f);

        g.setColour (theme::text.withMultipliedAlpha (alpha));
        g.setFont (theme::font (12.5f));
        g.drawText (text, r.withTrimmedLeft (22.0f).toNearestInt(), juce::Justification::centredLeft, true);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { started = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (started || e.getDistanceFromDragStart() < 6 || ! isEnabled())
            return;
        started = true;
        juce::StringArray files;
        if (makeFiles)
            files = makeFiles();
        else if (const auto file = makeFile(); file.existsAsFile())
            files.add (file.getFullPathName());
        if (! files.isEmpty())
            juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A plain click (no drag) just saves the file and shows it.
        if (! started && ! e.mouseWasDraggedSinceMouseDown() && onClick)
            onClick();
    }

    std::function<void()> onClick;

private:
    juce::String text;
    std::function<juce::File()> makeFile;
    bool hover = false, started = false;
};

// Small caption above a value or a group of controls.
inline void drawCaption (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& s)
{
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (s, r, juce::Justification::topLeft, false);
}

} // namespace choplab
