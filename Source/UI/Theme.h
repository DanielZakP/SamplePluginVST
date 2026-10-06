#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab::theme
{
// Graphite panels, one amber accent, a green waveform. Panels are drawn like hardware: flat
// fills, a dark edge and a faint top highlight, small corner radii.
inline const juce::Colour background { 0xff1b1d20 };
inline const juce::Colour panel { 0xff26292d };
inline const juce::Colour panelRaised { 0xff303338 };
inline const juce::Colour inset { 0xff151719 };     // waveform, grid, read-outs
inline const juce::Colour outline { 0xff3a3e44 };
inline const juce::Colour edge { 0xff0f1012 };      // the dark line around panels and controls
inline const juce::Colour text { 0xffdcdddf };
inline const juce::Colour textDim { 0xff979ba3 };
inline const juce::Colour textFaint { 0xff62676e };
inline const juce::Colour accent { 0xffe39b3b };
inline const juce::Colour wave { 0xff8fbb98 };
inline const juce::Colour good { 0xff8ec47b };
inline const juce::Colour warn { 0xffd9b05a };
inline const juce::Colour error { 0xffdf6a5d };
inline constexpr float radius = 2.5f;

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}

inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = panel)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (fill.brighter (0.07f));
    g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + radius, r.getRight() - radius);
    g.setColour (edge);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

inline void drawInset (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (inset);
    g.fillRoundedRectangle (r, radius);
    g.setColour (edge);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    g.setColour (panel.brighter (0.05f));
    g.drawHorizontalLine ((int) r.getBottom(), r.getX() + radius, r.getRight() - radius);
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
       #if JUCE_WINDOWS
        setDefaultSansSerifTypefaceName ("Segoe UI"); // the Windows UI font, not JUCE's default Verdana
       #endif

        setColour (juce::TextButton::buttonColourId, panelRaised);
        setColour (juce::TextButton::buttonOnColourId, accent.darker (0.05f));
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::ComboBox::backgroundColourId, inset);
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
        setColour (juce::TextEditor::backgroundColourId, inset);
        setColour (juce::TextEditor::outlineColourId, edge);
        setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.7f));
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, textFaint);
        setColour (juce::ListBox::backgroundColourId, inset);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TableHeaderComponent::backgroundColourId, panelRaised);
        setColour (juce::TableHeaderComponent::textColourId, textDim);
        setColour (juce::TableHeaderComponent::outlineColourId, outline);
        setColour (juce::ScrollBar::thumbColourId, outline.brighter (0.15f));
        setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, outline);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return font (juce::jmin (13.0f, (float) buttonHeight * 0.48f));
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
        auto r = b.getLocalBounds().toFloat();
        const bool on = b.getToggleState();
        auto c = on ? findColour (juce::TextButton::buttonOnColourId) : backgroundColour;
        if (down)
            c = c.darker (0.12f);
        else if (highlighted)
            c = c.brighter (0.06f);
        if (! b.isEnabled())
            c = c.withMultipliedAlpha (0.45f);
        g.setGradientFill (juce::ColourGradient::vertical (c.brighter (on ? 0.0f : 0.05f), r.getY(), c.darker (on ? 0.08f : 0.1f), r.getBottom()));
        g.fillRoundedRectangle (r, radius);
        g.setColour (edge.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
        if (! on && ! down)
        {
            g.setColour (juce::Colours::white.withAlpha (b.isEnabled() ? 0.05f : 0.02f));
            g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + radius, r.getRight() - radius);
        }
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float arcR = r - 1.5f;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (edge);
        g.strokePath (track, juce::PathStrokeType (3.0f));

        // Bipolar knobs (pitch, gain) fill from the centre; others from the start.
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float zeroPos = bipolar ? (float) s.valueToProportionOfLength (0.0) : 0.0f;
        const float fromAngle = startAngle + zeroPos * (endAngle - startAngle);
        const float toAngle = startAngle + pos * (endAngle - startAngle);
        if (std::abs (toAngle - fromAngle) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (fromAngle, toAngle), juce::jmax (fromAngle, toAngle), true);
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (s.isEnabled() ? 0.9f : 0.4f));
            g.strokePath (value, juce::PathStrokeType (2.0f));
        }

        // A machined cap: dark ring, lighter top, a line for the pointer.
        const float capR = arcR - 4.5f;
        g.setColour (edge);
        g.fillEllipse (centre.x - capR - 1.0f, centre.y - capR - 1.0f, (capR + 1.0f) * 2.0f, (capR + 1.0f) * 2.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4e55), centre.x, centre.y - capR, juce::Colour (0xff2a2d31), centre.x,
                                                 centre.y + capR, false));
        g.fillEllipse (centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f);
        juce::Path pointer;
        pointer.addRectangle (-1.0f, -capR + 1.5f, 2.0f, capR * 0.55f);
        pointer.applyTransform (juce::AffineTransform::rotation (toAngle).translated (centre));
        g.setColour (s.isEnabled() ? text : textFaint);
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
        g.setColour (edge);
        g.fillRect ((float) x, cy - 2.0f, (float) width, 4.0f);
        g.setColour (accent.withMultipliedAlpha (s.isEnabled() ? 0.85f : 0.35f));
        g.fillRect ((float) x + 1.0f, cy - 1.0f, sliderPos - (float) x - 1.0f, 2.0f);
        // Fader cap
        const auto cap = juce::Rectangle<float> (sliderPos - 4.0f, cy - 8.0f, 8.0f, 16.0f);
        g.setGradientFill (juce::ColourGradient::vertical (juce::Colour (0xff53575e), cap.getY(), juce::Colour (0xff33363b), cap.getBottom()));
        g.fillRoundedRectangle (cap, 1.5f);
        g.setColour (edge);
        g.drawRoundedRectangle (cap.reduced (0.5f), 1.5f, 1.0f);
        g.setColour (text.withAlpha (0.7f));
        g.drawVerticalLine ((int) sliderPos, cap.getY() + 3.0f, cap.getBottom() - 3.0f);
    }

    // An LED and its label instead of a phone-style switch.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) override
    {
        auto r = b.getLocalBounds().toFloat();
        const float size = 11.0f;
        const auto box = juce::Rectangle<float> (r.getX() + 1.0f, r.getCentreY() - size * 0.5f, size, size);
        const bool on = b.getToggleState();
        const float alpha = b.isEnabled() ? 1.0f : 0.45f;
        g.setColour (inset.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (box, 1.5f);
        g.setColour ((highlighted ? outline.brighter (0.2f) : edge).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (box.reduced (0.5f), 1.5f, 1.0f);
        if (on)
        {
            g.setColour (accent.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (box.reduced (2.5f), 1.0f);
        }
        g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.setFont (font (12.5f));
        g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (size + 8.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
        g.fillRoundedRectangle (r, radius);
        g.setColour (box.hasKeyboardFocus (true) ? accent.withAlpha (0.7f) : edge);
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
        juce::Path arrow;
        const float ax = (float) width - 11.0f, ay = (float) height * 0.5f;
        arrow.addTriangle (ax - 3.5f, ay - 1.5f, ax + 3.5f, ay - 1.5f, ax, ay + 2.5f);
        g.setColour (textDim.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
        g.fillPath (arrow);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (12.5f); }
    juce::Font getPopupMenuFont() override { return font (13.0f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (5, 1, box.getWidth() - 20, box.getHeight() - 2);
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
        g.setFont (font (12.0f));
        g.drawText (name, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }
};
} // namespace choplab::theme
