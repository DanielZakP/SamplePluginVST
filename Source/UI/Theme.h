#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab::theme
{
inline const juce::Colour background { 0xff111316 };
inline const juce::Colour panel { 0xff181b1f };
inline const juce::Colour panelRaised { 0xff20242a };
inline const juce::Colour outline { 0xff2c3138 };
inline const juce::Colour text { 0xffe7e9ec };
inline const juce::Colour textDim { 0xff8a919b };
inline const juce::Colour textFaint { 0xff5c636c };
inline const juce::Colour accent { 0xffff8a4c };
inline const juce::Colour good { 0xff6bd68a };
inline const juce::Colour warn { 0xfff5c451 };

// Chop colours cycle through this palette, like pads on a drum machine.
inline juce::Colour sliceColour (int index)
{
    static const juce::Colour palette[] { juce::Colour (0xffff8a4c), juce::Colour (0xfff5c451), juce::Colour (0xff6bd68a),
                                          juce::Colour (0xff4ccfc0), juce::Colour (0xff5aa9ff), juce::Colour (0xff9b8cff),
                                          juce::Colour (0xfff27bc0), juce::Colour (0xffff6b6b) };
    return palette[(index % 8 + 8) % 8];
}

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
        scheme.setUIColour (ColourScheme::windowBackground, background);
        scheme.setUIColour (ColourScheme::widgetBackground, panelRaised);
        scheme.setUIColour (ColourScheme::menuBackground, panelRaised);
        scheme.setUIColour (ColourScheme::outline, outline);
        scheme.setUIColour (ColourScheme::defaultText, text);
        scheme.setUIColour (ColourScheme::defaultFill, accent);
        scheme.setUIColour (ColourScheme::highlightedText, juce::Colours::black);
        scheme.setUIColour (ColourScheme::highlightedFill, accent);
        scheme.setUIColour (ColourScheme::menuText, text);
        setColourScheme (scheme);

        setColour (juce::TextButton::buttonColourId, panelRaised);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::ComboBox::backgroundColourId, panelRaised);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::arrowColourId, textDim);
        setColour (juce::PopupMenu::backgroundColourId, panelRaised);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
        setColour (juce::Slider::thumbColourId, accent);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, outline);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, outline);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::Label::textColourId, text);
        setColour (juce::Label::textWhenEditingColourId, text);
        setColour (juce::Label::backgroundWhenEditingColourId, panelRaised);
        setColour (juce::Label::outlineWhenEditingColourId, accent);
        setColour (juce::TextEditor::backgroundColourId, panelRaised);
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, textFaint);
        setColour (juce::ListBox::backgroundColourId, panel);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TableHeaderComponent::backgroundColourId, panelRaised);
        setColour (juce::TableHeaderComponent::textColourId, textDim);
        setColour (juce::TableHeaderComponent::outlineColourId, outline);
        setColour (juce::ScrollBar::thumbColourId, outline.brighter (0.2f));
        setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, outline);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return font (juce::jmin (14.0f, (float) buttonHeight * 0.5f));
    }

    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::textColourId, text);
        l->setFont (font (12.5f));
        return l;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour, bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? findColour (juce::TextButton::buttonOnColourId) : backgroundColour;
        if (! b.isEnabled())
            c = c.withMultipliedAlpha (0.4f);
        else if (down)
            c = c.brighter (0.15f);
        else if (highlighted)
            c = c.brighter (0.07f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 5.0f);
        if (! b.getToggleState())
        {
            g.setColour (outline);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float lineW = juce::jmax (2.5f, radius * 0.16f);
        const float arcR = radius - lineW * 0.5f;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (outline);
        g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Bipolar knobs (pitch, gain) fill from the centre; others from the start.
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float zeroPos = bipolar ? (float) s.valueToProportionOfLength (0.0) : 0.0f;
        const float fromAngle = startAngle + zeroPos * (endAngle - startAngle);
        const float toAngle = startAngle + pos * (endAngle - startAngle);
        if (std::abs (toAngle - fromAngle) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (fromAngle, toAngle), juce::jmax (fromAngle, toAngle), true);
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
            g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float knobR = arcR - lineW * 1.2f;
        g.setColour (panelRaised.brighter (0.05f));
        g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.25f, -knobR, 2.5f, knobR * 0.5f, 1.0f);
        pointer.applyTransform (juce::AffineTransform::rotation (toAngle).translated (centre));
        g.setColour (text);
        g.fillPath (pointer);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, s);
            return;
        }
        const float cy = (float) y + (float) height * 0.5f;
        g.setColour (outline);
        g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) width, 4.0f, 2.0f);
        g.setColour (accent.withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
        g.fillRoundedRectangle ((float) x, cy - 2.0f, sliderPos - (float) x, 4.0f, 2.0f);
        g.setColour (text);
        g.fillEllipse (sliderPos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) override
    {
        auto r = b.getLocalBounds().toFloat();
        const float h = juce::jmin (16.0f, r.getHeight() - 4.0f);
        auto sw = juce::Rectangle<float> (r.getX() + 1.0f, r.getCentreY() - h * 0.5f, h * 1.8f, h);
        const bool on = b.getToggleState();
        g.setColour ((on ? accent : outline).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f).brighter (highlighted ? 0.08f : 0.0f));
        g.fillRoundedRectangle (sw, h * 0.5f);
        const float d = h - 4.0f;
        g.setColour (on ? juce::Colours::black.withAlpha (0.8f) : text);
        g.fillEllipse (on ? sw.getRight() - d - 2.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, d, d);

        g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.setFont (font (13.0f));
        g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (sw.getWidth() + 8.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (box.hasKeyboardFocus (true) ? accent : outline);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        juce::Path arrow;
        const float ax = (float) width - 14.0f, ay = (float) height * 0.5f;
        arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
        g.setColour (textDim);
        g.fillPath (arrow);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.5f); }
    juce::Font getPopupMenuFont() override { return font (14.0f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 26, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header) override
    {
        g.fillAll (panelRaised);
        g.setColour (outline);
        g.fillRect (0, header.getHeight() - 1, header.getWidth(), 1);
    }

    void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& name, int, int width, int height,
                                bool, bool, int) override
    {
        g.setColour (textDim);
        g.setFont (font (12.0f, true));
        g.drawText (name.toUpperCase(), 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }
};
} // namespace choplab::theme
