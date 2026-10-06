#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace choplab
{

// Piano roll where every row is a chop, labelled with its number, note, label and lyrics.
// Mouse works like FL Studio: click to add, right-click (or right-drag) to delete, drag a note to
// move it, drag its right edge to resize, Ctrl+drag to select, Ctrl+wheel to zoom, Alt to ignore snap.
// The strip along the bottom of a note is its start slider: drag it to start that note later in
// its chop (Shift for fine steps, double-click to reset). The tag at the end of each row picks
// which stem that chop plays.
class PatternView : public juce::Component,
                    public juce::TooltipClient,
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
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::String getTooltip() override;

private:
    enum class Drag
    {
        none,
        move,
        resize,
        select,
        erase,
        velocity,
        offset
    };

    // Loudness envelope of a sample (or stem), for drawing chops inside notes.
    struct Peaks
    {
        std::shared_ptr<const SampleData> source;
        std::vector<float> level; // max |sample| per block
    };
    static constexpr int kPeakBlock = 128;
    const Peaks& peaksFor (const std::shared_ptr<const SampleData>&);

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
    juce::Rectangle<float> offsetTrack (juce::Rectangle<float> note) const;
    bool onOffsetTrack (int note, juce::Point<float>) const;
    juce::Rectangle<float> stemTag (int chop) const; // in component coordinates
    void showStemMenu (int chop);
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
    void drawNoteWave (juce::Graphics&, const PatternNote&, juce::Rectangle<float>);
    void drawVelocity (juce::Graphics&);

    ChopLabProcessor& proc;
    Pattern working;
    std::vector<bool> selected;
    std::vector<PatternNote> clipboard;

    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, fillButton { "Start from sample order" }, clearButton { "Clear" };
    juce::ComboBox lengthBox, snapBox;
    DragOutButton dragMidi, dragAudio;
    juce::ScrollBar hScroll { false }, vScroll { true };

    juce::Rectangle<int> toolbar, headerArea, rulerArea, gridArea, velocityArea, lengthCaption, snapCaption;
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
    double offsetAtGrab = 0.0;
    std::vector<Peaks> peaks;
    int lastSample = -2;
    bool fittedForSample = false;
};

} // namespace choplab
