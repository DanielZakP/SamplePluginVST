#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace choplab
{

// Piano roll where every row is a chop, labelled with its number, note, label and lyrics.
// Mouse works like FL Studio: click to add, right-click (or right-drag) to delete, drag a note to
// move it, drag its right edge to resize, Ctrl+drag to select, Ctrl+wheel to zoom, Alt to ignore snap.
class PatternView : public juce::Component,
                    private juce::Timer,
                    private juce::ScrollBar::Listener
{
public:
    explicit PatternView (ChopLabProcessor&);
    ~PatternView() override;

    void documentChanged();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag
    {
        none,
        move,
        resize,
        select,
        erase,
        velocity
    };

    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

    // geometry
    int numRows() const;
    float beatToX (double beat) const;
    double xToBeat (float x) const;
    int rowAtY (float y) const; // chop index, or -1
    float yForChop (int chop) const;
    juce::Rectangle<float> noteBounds (const PatternNote&) const;
    int noteAt (juce::Point<float>) const;
    double snap() const;
    double snapDown (double beat, bool fine) const;
    double snapNearest (double beat, bool fine) const;
    double defaultLength (int chop) const;
    void fitToWidth();
    void updateScrollbars();
    void clampScroll();

    // editing
    void commit (bool newEdit);
    void deleteSelected();
    void selectOnly (int index);
    juce::String rowText (int chop) const;
    juce::String noteText (int chop) const;
    void drawGrid (juce::Graphics&);
    void drawRows (juce::Graphics&);
    void drawNotes (juce::Graphics&);
    void drawVelocity (juce::Graphics&);

    ChopLabProcessor& proc;
    Pattern working;
    std::vector<bool> selected;
    std::vector<PatternNote> clipboard;

    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, fillButton { "Start from sample order" }, clearButton { "Clear" };
    juce::ComboBox lengthBox, snapBox;
    DragOutButton dragMidi;
    juce::ScrollBar hScroll { false }, vScroll { true };

    juce::Rectangle<int> toolbar, headerArea, rulerArea, gridArea, velocityArea, lengthCaption, snapCaption, hintArea;
    double pxPerBeat = 40.0, scrollBeats = 0.0;
    float scrollY = 0.0f;
    static constexpr float rowHeight = 24.0f;

    Drag drag = Drag::none;
    bool dragCommitted = false;
    int grabbedNote = -1;
    double grabBeat = 0.0;
    int grabRow = 0;
    std::vector<PatternNote> dragOriginal;
    juce::Rectangle<float> selectionBox;
    int lastSample = -2;
    bool fittedForSample = false;
};

} // namespace choplab
