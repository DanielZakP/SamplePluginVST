#include "WaveformView.h"
#include "Theme.h"

namespace choplab
{

namespace
{
constexpr int kRulerHeight = 20;
constexpr int kScrollHeight = 10;
constexpr int kLabelStrip = 20;
} // namespace

WaveformView::WaveformView (ChopLabProcessor& p) : proc (p)
{
    addAndMakeVisible (scrollbar);
    scrollbar.addListener (this);
    scrollbar.setAutoHide (false);
    setWantsKeyboardFocus (false);
    startTimerHz (30);
}

WaveformView::~WaveformView()
{
    scrollbar.removeListener (this);
}

void WaveformView::documentChanged()
{
    const auto& d = proc.doc();
    if (d.sample.get() != peaksFor)
    {
        rebuildPeaks();
        zoomToFit();
    }
    repaint();
}

void WaveformView::rebuildPeaks()
{
    const auto& d = proc.doc();
    peaksFor = d.sample.get();
    peakMin.clear();
    peakMax.clear();
    if (! d.hasSample())
        return;

    const auto& a = d.sample->audio;
    const int n = a.getNumSamples();
    const int blocks = (n + kPeakBlock - 1) / kPeakBlock;
    peakMin.assign ((size_t) blocks, 0.0f);
    peakMax.assign ((size_t) blocks, 0.0f);
    for (int c = 0; c < a.getNumChannels(); ++c)
    {
        const float* data = a.getReadPointer (c);
        for (int b = 0; b < blocks; ++b)
        {
            const int s0 = b * kPeakBlock, s1 = juce::jmin (n, s0 + kPeakBlock);
            float lo = peakMin[(size_t) b], hi = peakMax[(size_t) b];
            for (int i = s0; i < s1; ++i)
            {
                lo = juce::jmin (lo, data[i]);
                hi = juce::jmax (hi, data[i]);
            }
            peakMin[(size_t) b] = lo;
            peakMax[(size_t) b] = hi;
        }
    }
}

void WaveformView::zoomToFit()
{
    setView (0.0, (double) juce::jmax ((juce::int64) 1, proc.doc().length()));
}

void WaveformView::setView (double start, double length)
{
    const double total = (double) juce::jmax ((juce::int64) 1, proc.doc().length());
    const double minLength = juce::jmin (total, juce::jmax (64.0, proc.doc().sampleRate() * 0.01));
    viewLength = juce::jlimit (minLength, total, length);
    viewStart = juce::jlimit (0.0, total - viewLength, start);
    updateScrollbar();
    repaint();
}

void WaveformView::updateScrollbar()
{
    const double total = (double) juce::jmax ((juce::int64) 1, proc.doc().length());
    scrollbar.setRangeLimits (0.0, total, juce::dontSendNotification);
    scrollbar.setCurrentRange (viewStart, viewLength, juce::dontSendNotification);
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    viewStart = newRangeStart;
    repaint();
}

void WaveformView::resized()
{
    scrollbar.setBounds (getLocalBounds().removeFromBottom (kScrollHeight).reduced (2, 1));
}

juce::Rectangle<int> WaveformView::rulerArea() const
{
    return getLocalBounds().removeFromTop (kRulerHeight);
}

juce::Rectangle<int> WaveformView::waveArea() const
{
    auto r = getLocalBounds();
    r.removeFromTop (kRulerHeight);
    r.removeFromBottom (kScrollHeight);
    return r;
}

float WaveformView::sampleToX (double sample) const
{
    const auto w = waveArea();
    return (float) w.getX() + (float) ((sample - viewStart) / viewLength * w.getWidth());
}

juce::int64 WaveformView::xToSample (float x) const
{
    const auto w = waveArea();
    const double s = viewStart + (double) (x - (float) w.getX()) / juce::jmax (1, w.getWidth()) * viewLength;
    return juce::jlimit ((juce::int64) 0, proc.doc().length(), (juce::int64) std::llround (s));
}

int WaveformView::markerNear (float x) const
{
    const auto& slices = proc.doc().slices;
    int best = -1;
    float bestDist = 6.0f;
    for (int i = 1; i < (int) slices.size(); ++i)
    {
        const float d = std::abs (sampleToX ((double) slices[(size_t) i].start) - x);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int WaveformView::chopAt (juce::int64 position) const
{
    const auto& slices = proc.doc().slices;
    for (int i = 0; i < (int) slices.size(); ++i)
        if (position >= slices[(size_t) i].start && position < slices[(size_t) i].end)
            return i;
    return slices.empty() ? -1 : (int) slices.size() - 1;
}

juce::int64 WaveformView::snapToOnset (juce::int64 position) const
{
    const auto& d = proc.doc();
    const auto window = (juce::int64) (d.sampleRate() * 0.02);
    juce::int64 best = position, bestDist = window;
    for (const auto& o : d.onsets)
    {
        const auto dist = std::abs (o.position - position);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = o.position;
        }
    }
    return best;
}

//==============================================================================
void WaveformView::drawGrid (juce::Graphics& g, juce::Rectangle<int> wave, juce::Rectangle<int> ruler)
{
    const auto& d = proc.doc();
    if (d.bpm <= 0.0)
        return;

    const double sr = d.sampleRate();
    const double unitSeconds = 60.0 / d.bpm * 4.0 / d.timeSig.denominator;
    const int unitsPerBar = juce::jmax (1, d.timeSig.numerator);
    const double pxPerUnit = unitSeconds * sr / viewLength * wave.getWidth();
    const double pxPer16th = 60.0 / d.bpm / 4.0 * sr / viewLength * wave.getWidth();
    const double viewStartSec = viewStart / sr, viewEndSec = (viewStart + viewLength) / sr;

    // 16th lines when zoomed in far enough
    if (pxPer16th > 14.0)
    {
        g.setColour (theme::outline.withAlpha (0.35f));
        const double step = 60.0 / d.bpm / 4.0;
        for (auto k = (juce::int64) std::floor ((viewStartSec - d.downbeatSeconds) / step);; ++k)
        {
            const double t = d.downbeatSeconds + (double) k * step;
            if (t > viewEndSec)
                break;
            g.drawVerticalLine ((int) sampleToX (t * sr), (float) wave.getY(), (float) wave.getBottom());
        }
    }

    // Bars only, or bars plus beats, depending on zoom; bar numbers thinned out to stay readable.
    const bool showBeats = pxPerUnit > 7.0;
    const double pxPerBar = pxPerUnit * unitsPerBar;
    const int barLabelEvery = pxPerBar > 36.0 ? 1 : pxPerBar > 18.0 ? 2 : pxPerBar > 9.0 ? 4 : 8;

    g.setFont (theme::font (11.0f));
    for (auto k = (juce::int64) std::floor ((viewStartSec - d.downbeatSeconds) / unitSeconds);; ++k)
    {
        const double t = d.downbeatSeconds + (double) k * unitSeconds;
        if (t > viewEndSec)
            break;
        const bool isBar = ((k % unitsPerBar) + unitsPerBar) % unitsPerBar == 0;
        if (! isBar && ! showBeats)
            continue;
        const int x = (int) sampleToX (t * sr);
        const auto bar = (int) (k >= 0 ? k / unitsPerBar : -((-k + unitsPerBar - 1) / unitsPerBar)) + 1;

        g.setColour (isBar ? theme::outline.brighter (0.35f) : theme::outline);
        g.drawVerticalLine (x, (float) wave.getY(), (float) wave.getBottom());

        g.setColour (isBar ? theme::textDim : theme::outline.brighter (0.2f));
        g.drawVerticalLine (x, (float) ruler.getBottom() - (isBar ? 8.0f : 4.0f), (float) ruler.getBottom());
        if (isBar && (bar - 1) % barLabelEvery == 0)
        {
            g.setColour (bar == 1 ? theme::accent : theme::textDim);
            g.drawText (bar >= 1 ? juce::String (bar) : juce::String ("pickup"), x + 3, ruler.getY() + 1, 60, ruler.getHeight() - 4,
                        juce::Justification::centredLeft, false);
        }
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto& d = proc.doc();
    const auto wave = waveArea();
    const auto ruler = rulerArea();

    g.setColour (theme::panel);
    g.fillRect (getLocalBounds());
    g.setColour (theme::panelRaised);
    g.fillRect (ruler);

    if (! d.hasSample())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (17.0f));
        g.drawText (proc.isAnalysing() ? "Analyzing..." : "Drop a sample here (WAV, AIFF, FLAC, MP3, OGG)", wave, juce::Justification::centred);
        return;
    }

    const auto& slices = d.slices;
    const int selected = proc.selectedSlice;

    // Chop backgrounds
    for (int i = 0; i < (int) slices.size(); ++i)
    {
        const auto& s = slices[(size_t) i];
        const float x0 = juce::jmax ((float) wave.getX(), sampleToX ((double) s.start));
        const float x1 = juce::jmin ((float) wave.getRight(), sampleToX ((double) s.end));
        if (x1 <= x0)
            continue;
        const auto c = theme::sliceColour (i);
        g.setColour (c.withAlpha (i == selected ? 0.16f : 0.045f));
        g.fillRect (x0, (float) wave.getY(), x1 - x0, (float) wave.getHeight());
    }

    drawGrid (g, wave, ruler);

    // Waveform, coloured per chop
    const float mid = (float) wave.getCentreY();
    const float halfH = (float) (wave.getHeight() - kLabelStrip - 6) * 0.5f;
    const float centre = mid + kLabelStrip * 0.5f;
    const double spp = viewLength / juce::jmax (1, wave.getWidth());
    const auto& audio = d.sample->audio;
    int chop = juce::jmax (0, chopAt ((juce::int64) viewStart));

    for (int x = wave.getX(); x < wave.getRight(); ++x)
    {
        const double s0 = viewStart + (x - wave.getX()) * spp;
        const double s1 = s0 + spp;
        float lo = 0.0f, hi = 0.0f;
        if (spp >= kPeakBlock && ! peakMin.empty())
        {
            const int b0 = juce::jlimit (0, (int) peakMin.size() - 1, (int) (s0 / kPeakBlock));
            const int b1 = juce::jlimit (b0, (int) peakMin.size() - 1, (int) (s1 / kPeakBlock));
            lo = peakMin[(size_t) b0];
            hi = peakMax[(size_t) b0];
            for (int b = b0 + 1; b <= b1; ++b)
            {
                lo = juce::jmin (lo, peakMin[(size_t) b]);
                hi = juce::jmax (hi, peakMax[(size_t) b]);
            }
        }
        else
        {
            const int i0 = juce::jlimit (0, audio.getNumSamples() - 1, (int) std::floor (s0));
            const int i1 = juce::jlimit (i0, audio.getNumSamples() - 1, (int) std::ceil (s1));
            lo = hi = audio.getSample (0, i0);
            for (int c = 0; c < audio.getNumChannels(); ++c)
                for (int i = i0; i <= i1; ++i)
                {
                    lo = juce::jmin (lo, audio.getSample (c, i));
                    hi = juce::jmax (hi, audio.getSample (c, i));
                }
        }

        while (chop + 1 < (int) slices.size() && s0 >= (double) slices[(size_t) chop + 1].start)
            ++chop;
        const auto colour = theme::sliceColour (chop).withAlpha (chop == selected ? 1.0f : 0.72f);
        g.setColour (colour);
        const float y0 = centre - juce::jlimit (-1.0f, 1.0f, hi) * halfH;
        const float y1 = centre - juce::jlimit (-1.0f, 1.0f, lo) * halfH;
        g.fillRect ((float) x, y0, 1.0f, juce::jmax (1.0f, y1 - y0));
    }

    // Markers and chop tags
    g.setFont (theme::font (11.5f, true));
    for (int i = 0; i < (int) slices.size(); ++i)
    {
        const auto& s = slices[(size_t) i];
        const float x = sampleToX ((double) s.start);
        const float xEnd = sampleToX ((double) s.end);
        if (xEnd < (float) wave.getX() || x > (float) wave.getRight())
            continue;
        const auto c = theme::sliceColour (i);

        if (i > 0)
        {
            const bool hot = i == hoverMarker || i == draggingMarker;
            g.setColour (hot ? theme::text : c.withAlpha (0.9f));
            g.fillRect (x - (hot ? 1.0f : 0.5f), (float) wave.getY(), hot ? 2.0f : 1.0f, (float) wave.getHeight());
            juce::Path handle;
            handle.addTriangle (x - 5.0f, (float) wave.getY(), x + 5.0f, (float) wave.getY(), x, (float) wave.getY() + 6.0f);
            g.fillPath (handle);
        }

        const float tagX = juce::jmax ((float) wave.getX(), x) + 3.0f;
        const float room = xEnd - tagX - 3.0f;
        if (room < (x < (float) wave.getX() ? 40.0f : 14.0f))
            continue;
        const juce::String number (i + 1);
        const float numW = juce::jmin (room, (float) number.length() * 7.0f + 8.0f);
        const juce::Rectangle<float> tag (tagX, (float) wave.getY() + 3.0f, numW, 15.0f);
        g.setColour (c.withAlpha (i == selected ? 1.0f : 0.85f));
        g.fillRoundedRectangle (tag, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawText (number, tag, juce::Justification::centred, false);

        const auto& label = s.settings.label;
        if (label.isNotEmpty() && room - numW > 20.0f)
        {
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f));
            g.drawText (label, juce::Rectangle<float> (tag.getRight() + 4.0f, tag.getY(), room - numW - 4.0f, 15.0f),
                        juce::Justification::centredLeft, true);
            g.setFont (theme::font (11.5f, true));
        }
    }

    // Lyrics along the bottom, each word where it's sung (skipping words that would overlap)
    if (! d.lyrics.words.empty())
    {
        const auto wordFont = theme::font (12.0f);
        g.setFont (wordFont);
        const float y = (float) wave.getBottom() - 19.0f;
        float lastRight = -1.0e9f;
        for (const auto& w : d.lyrics.words)
        {
            const float x = sampleToX (w.start * d.sampleRate());
            if (x > (float) wave.getRight())
                break;
            const float width = (float) juce::GlyphArrangement::getStringWidthInt (wordFont, w.text) + 8.0f;
            if (x + width < (float) wave.getX() || x < lastRight + 3.0f)
                continue;
            const juce::Rectangle<float> box (x, y, width, 16.0f);
            g.setColour (theme::background.withAlpha (0.75f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (theme::text.withAlpha (0.9f));
            g.drawText (w.text, box, juce::Justification::centred, false);
            lastRight = box.getRight();
        }
    }

    // Playheads
    g.setColour (theme::text);
    for (auto p : playing)
    {
        const float x = sampleToX ((double) p);
        if (x >= (float) wave.getX() && x <= (float) wave.getRight())
            g.fillRect (x - 0.5f, (float) wave.getY(), 1.5f, (float) wave.getHeight());
    }

    g.setColour (theme::outline);
    g.drawHorizontalLine (ruler.getBottom(), 0.0f, (float) getWidth());
}

//==============================================================================
void WaveformView::timerCallback()
{
    std::vector<juce::int64> now;
    proc.getPlayingPositions (now);
    if (now != playing)
    {
        playing = std::move (now);
        repaint (waveArea());
    }
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const int m = waveArea().contains (e.getPosition()) ? markerNear ((float) e.x) : -1;
    if (m != hoverMarker)
    {
        hoverMarker = m;
        repaint();
    }
    setMouseCursor (m >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (hoverMarker >= 0)
    {
        hoverMarker = -1;
        repaint();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    pressedChop = -1;
    dragExported = false;
    if (! proc.doc().hasSample() || ! waveArea().contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
    {
        showMenu (e);
        return;
    }

    const int marker = markerNear ((float) e.x);
    if (marker >= 0 && e.mods.isAltDown())
    {
        proc.removeMarker (marker);
        return;
    }
    if (marker >= 0)
    {
        draggingMarker = marker;
        return;
    }

    pressedChop = chopAt (xToSample ((float) e.x));
    if (pressedChop >= 0 && onChopClicked)
        onChopClicked (pressedChop);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingMarker >= 0)
    {
        proc.moveMarker (draggingMarker, xToSample ((float) e.x), false);
        return;
    }
    // Dragging a chop away from the waveform exports it and hands it to FL.
    if (pressedChop >= 0 && ! dragExported && e.getDistanceFromDragStart() > 12 && onChopDraggedOut)
    {
        dragExported = true;
        onChopDraggedOut (pressedChop);
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (draggingMarker >= 0)
    {
        proc.moveMarker (draggingMarker, xToSample ((float) e.x), true);
        draggingMarker = -1;
    }
    pressedChop = -1;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! proc.doc().hasSample() || ! waveArea().contains (e.getPosition()) || markerNear ((float) e.x) >= 0)
        return;
    proc.addMarker (e.mods.isShiftDown() ? xToSample ((float) e.x) : snapToOnset (xToSample ((float) e.x)));
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! proc.doc().hasSample())
        return;
    const bool horizontal = std::abs (w.deltaX) > std::abs (w.deltaY);
    if (horizontal || e.mods.isShiftDown())
    {
        const float delta = horizontal ? w.deltaX : w.deltaY;
        setView (viewStart - delta * viewLength * 0.5, viewLength);
        return;
    }
    const double anchor = (double) xToSample ((float) e.x);
    const double factor = std::exp (-w.deltaY * 1.6);
    const double newLength = viewLength * factor;
    const double frac = (anchor - viewStart) / viewLength;
    setView (anchor - frac * newLength, newLength);
}

void WaveformView::showMenu (const juce::MouseEvent& e)
{
    const auto pos = xToSample ((float) e.x);
    const int marker = markerNear ((float) e.x);
    const int chop = chopAt (pos);
    const auto& d = proc.doc();

    juce::PopupMenu m;
    m.addItem (1, "Add chop here");
    m.addItem (2, marker >= 0 ? "Remove this marker" : "Merge chop " + juce::String (chop + 1) + " into the previous one", marker >= 0 || chop > 0);
    m.addSeparator();
    m.addItem (3, "Set bar 1 at the start of chop " + juce::String (chop + 1));
    m.addItem (4, "Set bar 1 here");
    m.addSeparator();
    m.addItem (5, "Zoom to fit");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 }),
                     [safe = juce::Component::SafePointer<WaveformView> (this), pos, marker, chop,
                      startSeconds = chop >= 0 ? d.secondsAt (d.slices[(size_t) chop].start) : 0.0] (int r)
                     {
                         if (safe == nullptr)
                             return;
                         auto& p = safe->proc;
                         switch (r)
                         {
                             case 1: p.addMarker (safe->snapToOnset (pos)); break;
                             case 2: p.removeMarker (marker >= 0 ? marker : chop); break;
                             case 3: p.setDownbeat (startSeconds); break;
                             case 4: p.setDownbeat (p.doc().secondsAt (safe->snapToOnset (pos))); break;
                             case 5: safe->zoomToFit(); break;
                             default: break;
                         }
                     });
}

} // namespace choplab
