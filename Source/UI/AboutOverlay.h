#pragma once

#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

namespace choplab
{

// Version, licence and credits. The AGPL asks interactive programs to show this somewhere.
class AboutOverlay : public juce::Component
{
public:
    AboutOverlay()
    {
        source.setURL (juce::URL ("https://github.com/DanielZakP/SamplePluginVST"));
        source.setButtonText ("github.com/DanielZakP/SamplePluginVST");
        source.setFont (theme::font (14.0f), false, juce::Justification::centredLeft);
        source.setColour (juce::HyperlinkButton::textColourId, theme::accent);
        licence.setURL (juce::URL ("https://www.gnu.org/licenses/agpl-3.0.html"));
        licence.setButtonText ("GNU Affero General Public License v3");
        licence.setFont (theme::font (14.0f), false, juce::Justification::centredLeft);
        licence.setColour (juce::HyperlinkButton::textColourId, theme::accent);
        close.onClick = [this] { setVisible (false); };
        addAndMakeVisible (source);
        addAndMakeVisible (licence);
        addAndMakeVisible (close);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::background.withAlpha (0.75f));
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (panel.toFloat(), 10.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (panel.toFloat(), 10.0f, 1.0f);

        auto r = panel.reduced (24, 20);
        g.setFont (theme::font (22.0f, true));
        g.setColour (theme::accent);
        g.drawText ("CHOP", r.getX(), r.getY(), 64, 30, juce::Justification::centredLeft);
        g.setColour (theme::text);
        g.drawText ("LAB", r.getX() + 62, r.getY(), 60, 30, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText ("version " + juce::String (JucePlugin_VersionString), r.getX() + 124, r.getY(), 200, 30, juce::Justification::centredLeft);

        g.setColour (theme::text);
        g.setFont (theme::font (14.0f));
        const juce::String body =
            "Copyright (C) 2026 DanielZakP and contributors.\n\n"
            "ChopLab is free software: you can redistribute it and/or modify it under the terms of the "
            "GNU Affero General Public License as published by the Free Software Foundation, either version 3 "
            "of the License, or (at your option) any later version.\n\n"
            "It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the "
            "implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the license for details.\n\n"
            "Built with JUCE, whisper.cpp and OpenAI's Whisper models, demucs.cpp and Meta's Demucs models, Eigen, "
            "Signalsmith Stretch, the Steinberg VST3 SDK, "
            "FLAC, Ogg Vorbis, zlib, libpng, the Independent JPEG Group's libjpeg, HarfBuzz and SheenBidi. Their "
            "licenses and copyright notices are in THIRD_PARTY_NOTICES.md next to the plugin and in the source code.";
        g.drawFittedText (body, bodyArea, juce::Justification::topLeft, 20, 1.0f);

        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText ("SOURCE CODE", sourceCaption, juce::Justification::centredLeft);
        g.drawText ("LICENSE", licenceCaption, juce::Justification::centredLeft);
    }

    void resized() override
    {
        panel = getLocalBounds().withSizeKeepingCentre (juce::jmin (620, getWidth() - 40), juce::jmin (410, getHeight() - 40));
        auto r = panel.reduced (24, 20);
        close.setBounds (r.getRight() - 70, r.getY(), 70, 28);
        r.removeFromTop (46);
        bodyArea = r.removeFromTop (r.getHeight() - 64);
        auto row = r.removeFromTop (28);
        sourceCaption = row.removeFromLeft (100);
        source.setBounds (row);
        row = r.removeFromTop (28);
        licenceCaption = row.removeFromLeft (100);
        licence.setBounds (row);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panel.contains (e.getPosition()))
            setVisible (false);
    }

private:
    juce::HyperlinkButton source, licence;
    juce::TextButton close { "Close" };
    juce::Rectangle<int> panel, bodyArea, sourceCaption, licenceCaption;
};

} // namespace choplab
