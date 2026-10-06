#pragma once

#include "../PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab
{

// Waveform with chop markers, bar/beat grid and playheads.
//  click a chop: select + play     drag a marker: move it        drag a chop out: export it as WAV
//  double-click: add a chop        right-click: more options      wheel: zoom, shift+wheel: scroll
class WaveformView : public juce::Component,
                     private juce::Timer,
                     private juce::ScrollBar::Listener
{
public:
    explicit WaveformView (ChopLabProcessor&);
    ~WaveformView() override;

    void documentChanged();
    void zoomToFit();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

    std::function<void (int)> onChopClicked;
    std::function<void (int)> onChopDraggedOut;

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    void rebuildPeaks();
    void setView (double start, double length);
    void updateScrollbar();
    juce::Rectangle<int> waveArea() const;
    juce::Rectangle<int> rulerArea() const;
    float sampleToX (double sample) const;
    juce::int64 xToSample (float x) const;
    int markerNear (float x) const; // index of the chop whose start marker is under x (never 0)
    int chopAt (juce::int64 position) const;
    juce::int64 snapToOnset (juce::int64 position) const;
    void showMenu (const juce::MouseEvent&);
    void drawGrid (juce::Graphics&, juce::Rectangle<int> wave, juce::Rectangle<int> ruler);

    // Min/max per block of 64 samples, for the mix and any stems chops are set to play
    struct Peaks
    {
        std::shared_ptr<const SampleData> source;
        std::vector<float> lo, hi;
    };
    const Peaks& peaksOf (const std::shared_ptr<const SampleData>&);

    ChopLabProcessor& proc;
    static constexpr int kPeakBlock = 64;
    std::vector<Peaks> peaks;
    const SampleData* peaksFor = nullptr;

    double viewStart = 0.0, viewLength = 1.0;
    int draggingMarker = -1;
    int hoverMarker = -1;
    int pressedChop = -1;
    bool dragExported = false;
    std::vector<juce::int64> playing;
    juce::ScrollBar scrollbar { false };
};

} // namespace choplab
