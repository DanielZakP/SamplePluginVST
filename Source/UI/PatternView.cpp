#include "PatternView.h"
#include "Theme.h"

namespace choplab
{

namespace
{
struct Choice
{
    const char* name;
    double value;
};
// Snap in quarter-note beats; -1 = one bar
const Choice kSnaps[] { { "None", 0.0 }, { "1/32", 0.125 }, { "1/16", 0.25 }, { "1/8", 0.5 }, { "1/4 (beat)", 1.0 },
                        { "1/2", 2.0 },  { "Bar", -1.0 },   { "1/16 triplet", 1.0 / 6.0 }, { "1/8 triplet", 1.0 / 3.0 } };
const int kBars[] { 1, 2, 4, 8, 16, 32, 64 };
constexpr int kHeaderWidth = 260;
constexpr float kTrackHeight = 5.0f; // the start slider along the bottom of each note

bool isBlackKey (int note)
{
    const int n = (note % 12 + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

juce::String shortStemName (int stem)
{
    switch (stem)
    {
        case stemVocals: return "Vox";
        case stemDrums: return "Drums";
        case stemBass: return "Bass";
        case stemOther: return "Other";
        case stemInstrumental: return "Inst";
        default: return "Mix";
    }
}
constexpr int kRulerHeight = 20;
constexpr int kVelocityHeight = 60;
constexpr int kScroll = 10;
} // namespace

PatternView::PatternView (ChopLabProcessor& p)
    : proc (p),
      dragMidi ("Drag MIDI to FL", [this] { return proc.writePatternMidi (proc.getDragFolder()); }),
      dragAudio ("Drag audio", [this] { return proc.writePatternAudio (proc.getDragFolder()); })
{
    setWantsKeyboardFocus (true);

    playButton.onClick = [this] { proc.playPattern (true); };
    stopButton.onClick = [this] { proc.playPattern (false); };
    playButton.setTooltip ("Play the pattern in a loop at the project tempo (Space)");
    fillButton.setTooltip ("Fill the pattern with every chop in its original place, a starting point for flipping the sample");
    fillButton.onClick = [this] { proc.fillPatternFromSample(); };
    clearButton.onClick = [this]
    {
        auto p2 = proc.doc().pattern;
        p2.notes.clear();
        proc.setPattern (p2);
    };
    dragMidi.onClick = [this]
    {
        if (auto f = proc.writePatternMidi (proc.getDragFolder()); f.existsAsFile())
            f.revealToUser();
    };
    dragAudio.setTooltip ("Drag the pattern into FL's playlist as audio, exactly as it plays here (start offsets, stems, velocities). "
                          "One loop at the project tempo that loops seamlessly. Click to save it and show the file");
    dragAudio.onClick = [this]
    {
        if (auto f = proc.writePatternAudio (proc.getDragFolder()); f.existsAsFile())
            f.revealToUser();
    };

    for (int i = 0; i < (int) std::size (kBars); ++i)
        lengthBox.addItem (juce::String (kBars[i]) + (kBars[i] == 1 ? " bar" : " bars"), i + 1);
    lengthBox.setTooltip ("Pattern length");
    lengthBox.onChange = [this]
    {
        const int i = lengthBox.getSelectedId() - 1;
        if (i < 0)
            return;
        auto p2 = proc.doc().pattern;
        p2.lengthBeats = kBars[i] * barBeats (proc.doc().timeSig);
        proc.setPattern (p2);
        fitToWidth();
    };

    for (int i = 0; i < (int) std::size (kSnaps); ++i)
        snapBox.addItem (kSnaps[i].name, i + 1);
    snapBox.setTooltip ("Grid snap (hold Alt while dragging to ignore it)");
    snapBox.onChange = [this]
    {
        const int i = snapBox.getSelectedId() - 1;
        if (i < 0)
            return;
        auto p2 = proc.doc().pattern;
        p2.snapBeats = kSnaps[i].value < 0.0 ? barBeats (proc.doc().timeSig) : kSnaps[i].value;
        proc.setPattern (p2);
    };

    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &fillButton, &clearButton, &lengthBox, &snapBox, &dragMidi,
                                                             &dragAudio, &hScroll, &vScroll })
        addAndMakeVisible (c);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    vScroll.setAutoHide (false);
    startTimerHz (30);
}

PatternView::~PatternView()
{
    hScroll.removeListener (this);
    vScroll.removeListener (this);
}

//==============================================================================
void PatternView::documentChanged()
{
    const auto& d = proc.doc();
    if (drag == Drag::none)
    {
        working = d.pattern;
        selected.resize (working.notes.size(), false);
    }

    const int sampleId = (int) (juce::pointer_sized_int) d.mix().get();
    if (sampleId != lastSample)
    {
        lastSample = sampleId;
        fittedForSample = false;
        peaks.clear();
    }
    if (! fittedForSample && getWidth() > 0)
    {
        fitToWidth();
        fittedForSample = true;
        scrollY = 1.0e9f; // start at the bottom, where chop 1 is
        clampScroll();
    }

    const double bar = barBeats (d.timeSig);
    int lengthId = 0;
    for (int i = 0; i < (int) std::size (kBars); ++i)
        if (std::abs (kBars[i] * bar - working.lengthBeats) < 1.0e-6)
            lengthId = i + 1;
    if (lengthId > 0)
        lengthBox.setSelectedId (lengthId, juce::dontSendNotification);
    else
        lengthBox.setText (juce::String (working.lengthBeats / bar, 2) + " bars", juce::dontSendNotification);

    int snapId = 0;
    for (int i = 0; i < (int) std::size (kSnaps); ++i)
    {
        const double v = kSnaps[i].value < 0.0 ? bar : kSnaps[i].value;
        if (std::abs (v - working.snapBeats) < 1.0e-6 && snapId == 0)
            snapId = i + 1;
    }
    snapBox.setSelectedId (juce::jmax (1, snapId), juce::dontSendNotification);

    const bool has = d.hasSample();
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &fillButton, &clearButton, &lengthBox, &snapBox })
        c->setEnabled (has);
    dragMidi.setEnabled (has && ! working.notes.empty());
    dragAudio.setEnabled (has && ! working.notes.empty());
    bool anyOffset = false;
    for (const auto& n : working.notes)
        anyOffset = anyOffset || n.offset > 0.0;
    dragMidi.setTooltip (juce::String ("Drag into FL's piano roll or playlist. The notes play the same chops on this channel.")
                         + (anyOffset ? " Start offsets stay behind: MIDI has no way to carry them, so in FL those notes start at the top of the chop. "
                                       "Use Drag audio to keep them." : ""));
    playButton.setToggleState (proc.isPatternPlaying(), juce::dontSendNotification);
    updateScrollbars();
    repaint();
}

void PatternView::resized()
{
    auto r = getLocalBounds();
    toolbar = r.removeFromTop (40);
    {
        auto t = toolbar.reduced (12, 7);
        playButton.setBounds (t.removeFromLeft (60));
        t.removeFromLeft (4);
        stopButton.setBounds (t.removeFromLeft (52));
        t.removeFromLeft (14);
        lengthCaption = t.removeFromLeft (48);
        lengthBox.setBounds (t.removeFromLeft (90));
        t.removeFromLeft (10);
        snapCaption = t.removeFromLeft (36);
        snapBox.setBounds (t.removeFromLeft (110));
        t.removeFromLeft (14);
        fillButton.setBounds (t.removeFromLeft (172));
        t.removeFromLeft (4);
        clearButton.setBounds (t.removeFromLeft (56));
        dragMidi.setBounds (t.removeFromRight (140));
        t.removeFromRight (6);
        dragAudio.setBounds (t.removeFromRight (112));
    }

    r.removeFromTop (8);
    auto body = r;
    vScroll.setBounds (body.removeFromRight (kScroll).withTrimmedTop (kRulerHeight).withTrimmedBottom (kVelocityHeight + kScroll));
    hScroll.setBounds (body.removeFromBottom (kScroll).withTrimmedLeft (kHeaderWidth));
    headerArea = body.removeFromLeft (kHeaderWidth);
    rulerArea = body.removeFromTop (kRulerHeight);
    headerArea.removeFromTop (kRulerHeight);
    velocityArea = body.removeFromBottom (kVelocityHeight);
    headerArea.removeFromBottom (kVelocityHeight);
    gridArea = body;

    if (! fittedForSample && proc.doc().hasSample())
    {
        fitToWidth();
        fittedForSample = true;
        scrollY = 1.0e9f;
    }
    clampScroll();
    updateScrollbars();
}

//==============================================================================
int PatternView::numRows() const { return juce::jmax (1, (int) proc.doc().slices.size()); }

float PatternView::beatToX (double beat) const { return (float) gridArea.getX() + (float) ((beat - scrollBeats) * pxPerBeat); }

double PatternView::xToBeat (float x) const { return scrollBeats + (double) (x - (float) gridArea.getX()) / pxPerBeat; }

// Like any piano roll, higher notes (later chops) are higher up.
float PatternView::yForChop (int chop) const
{
    return (float) gridArea.getY() + (float) (numRows() - 1 - chop) * rowHeight - scrollY;
}

int PatternView::rowAtY (float y) const
{
    const int fromTop = (int) std::floor ((y - (float) gridArea.getY() + scrollY) / rowHeight);
    const int chop = numRows() - 1 - fromTop;
    return chop >= 0 && chop < (int) proc.doc().slices.size() ? chop : -1;
}

juce::Rectangle<float> PatternView::noteBounds (const PatternNote& n) const
{
    const float x0 = beatToX (n.start), x1 = beatToX (n.end());
    return { x0, yForChop (n.chop) + 1.0f, juce::jmax (3.0f, x1 - x0), rowHeight - 2.0f };
}

int PatternView::noteAt (juce::Point<float> p) const
{
    for (int i = (int) working.notes.size(); --i >= 0;)
        if (noteBounds (working.notes[(size_t) i]).contains (p))
            return i;
    return -1;
}

double PatternView::snap() const { return working.snapBeats; }

double PatternView::snapDown (double beat, bool fine) const
{
    const double s = snap();
    return (fine || s <= 0.0) ? beat : std::floor (beat / s + 1.0e-9) * s;
}

double PatternView::snapNearest (double beat, bool fine) const
{
    const double s = snap();
    return (fine || s <= 0.0) ? beat : std::round (beat / s) * s;
}

// New notes play the whole chop, rounded to the grid
double PatternView::defaultLength (int chop) const
{
    const auto& d = proc.doc();
    double len = chop >= 0 && chop < (int) d.slices.size() ? d.lengthInBeats (d.slices[(size_t) chop]) : 1.0;
    const double s = snap() > 0.0 ? snap() : 0.25;
    return juce::jmax (s, std::round (len / s) * s);
}

juce::Rectangle<float> PatternView::offsetTrack (juce::Rectangle<float> note) const
{
    return note.withTrimmedTop (note.getHeight() - kTrackHeight).withTrimmedRight (juce::jmin (6.0f, note.getWidth() * 0.25f));
}

bool PatternView::onOffsetTrack (int note, juce::Point<float> p) const
{
    if (note < 0 || note >= (int) working.notes.size())
        return false;
    const auto r = noteBounds (working.notes[(size_t) note]);
    return r.getWidth() >= 14.0f && offsetTrack (r).expanded (0.0f, 1.0f).contains (p);
}

juce::Rectangle<float> PatternView::stemTag (int chop) const
{
    const float y = yForChop (chop) - (float) gridArea.getY() + (float) headerArea.getY();
    return { (float) headerArea.getRight() - 50.0f, y + 4.0f, 42.0f, rowHeight - 8.0f };
}

void PatternView::showStemMenu (int chop)
{
    const auto& d = proc.doc();
    if (chop < 0 || chop >= (int) d.slices.size())
        return;
    const int current = d.slices[(size_t) chop].settings.stem;
    const bool haveStems = d.stems != nullptr;
    const auto phase = proc.getStemsStatus().phase;
    const bool separating = phase == ChopLabProcessor::StemsStatus::downloading || phase == ChopLabProcessor::StemsStatus::separating;

    juce::PopupMenu m;
    m.addSectionHeader ("Chop " + juce::String (chop + 1) + " plays");
    m.addItem (1, "Same as the sample (" + (d.source >= 0 ? stemName (d.source).toLowerCase() : juce::String ("full mix")) + ")", true, current < 0);
    m.addSeparator();
    for (int i = 0; i < kNumStems; ++i)
        m.addItem (10 + i, stemName (i) + " only", haveStems, current == i);
    if (! haveStems)
    {
        m.addSeparator();
        m.addItem (2, separating ? "Separating stems..." : "Separate stems", ! separating);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (stemTag (chop).toNearestInt())),
                     [safe = juce::Component::SafePointer<PatternView> (this), chop] (int result)
                     {
                         if (safe == nullptr || result == 0)
                             return;
                         auto& pr = safe->proc;
                         if (result == 2)
                         {
                             pr.separateStems();
                             return;
                         }
                         if (chop >= (int) pr.doc().slices.size())
                             return;
                         auto settings = pr.doc().slices[(size_t) chop].settings;
                         settings.stem = result >= 10 ? result - 10 : -1;
                         pr.setSliceSettings (chop, settings);
                     });
}

const PatternView::Peaks& PatternView::peaksFor (const std::shared_ptr<const SampleData>& source)
{
    for (const auto& p : peaks)
        if (p.source == source)
            return p;
    if (peaks.size() >= 8)
        peaks.erase (peaks.begin());

    Peaks p;
    p.source = source;
    const auto& a = source->audio;
    const int n = a.getNumSamples();
    p.level.assign ((size_t) ((n + kPeakBlock - 1) / kPeakBlock), 0.0f);
    for (int c = 0; c < a.getNumChannels(); ++c)
    {
        const float* data = a.getReadPointer (c);
        for (int i = 0; i < n; ++i)
        {
            auto& level = p.level[(size_t) (i / kPeakBlock)];
            level = juce::jmax (level, std::abs (data[i]));
        }
    }
    peaks.push_back (std::move (p));
    return peaks.back();
}

void PatternView::fitToWidth()
{
    const double len = juce::jmax (1.0, proc.doc().pattern.lengthBeats);
    pxPerBeat = juce::jlimit (6.0, 400.0, (gridArea.getWidth() - 8) / len);
    scrollBeats = 0.0;
    updateScrollbars();
    repaint();
}

void PatternView::clampScroll()
{
    const float content = (float) numRows() * rowHeight;
    scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, content - (float) gridArea.getHeight()), scrollY);
    const double visibleBeats = gridArea.getWidth() / pxPerBeat;
    scrollBeats = juce::jlimit (0.0, juce::jmax (0.0, working.lengthBeats + 4.0 - visibleBeats), scrollBeats);
}

void PatternView::updateScrollbars()
{
    const double visibleBeats = juce::jmax (1.0, gridArea.getWidth() / pxPerBeat);
    hScroll.setRangeLimits (0.0, juce::jmax (visibleBeats, working.lengthBeats + 4.0), juce::dontSendNotification);
    hScroll.setCurrentRange (scrollBeats, visibleBeats, juce::dontSendNotification);
    const double content = numRows() * (double) rowHeight;
    vScroll.setRangeLimits (0.0, juce::jmax (content, (double) gridArea.getHeight()), juce::dontSendNotification);
    vScroll.setCurrentRange (scrollY, gridArea.getHeight(), juce::dontSendNotification);
}

void PatternView::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hScroll)
        scrollBeats = start;
    else
        scrollY = (float) start;
    repaint();
}

void PatternView::timerCallback()
{
    if (proc.isPatternPlaying() != playButton.getToggleState())
        playButton.setToggleState (proc.isPatternPlaying(), juce::dontSendNotification);
    if (proc.isPatternPlaying())
        repaint (gridArea.getUnion (rulerArea));
}

//==============================================================================
juce::String PatternView::noteText (int chop) const
{
    const auto& d = proc.doc();
    if (chop < 0 || chop >= (int) d.slices.size())
        return {};
    const auto& label = d.slices[(size_t) chop].settings.label;
    const auto lyrics = d.lyricsFor (chop);
    if (label.isNotEmpty())
        return lyrics.isNotEmpty() ? label + ": " + lyrics : label;
    return lyrics.isNotEmpty() ? lyrics : juce::String (chop + 1);
}

void PatternView::drawRows (juce::Graphics& g)
{
    const auto& d = proc.doc();
    g.saveState();
    g.reduceClipRegion (headerArea);
    g.setColour (theme::panel);
    g.fillRect (headerArea);

    for (int chop = 0; chop < (int) d.slices.size(); ++chop)
    {
        const float y = yForChop (chop) - (float) gridArea.getY() + (float) headerArea.getY();
        if (y + rowHeight < (float) headerArea.getY() || y > (float) headerArea.getBottom())
            continue;
        auto row = juce::Rectangle<float> ((float) headerArea.getX(), y, (float) headerArea.getWidth(), rowHeight);
        const int note = d.noteForSlice (chop);
        if (chop == proc.selectedSlice)
            g.setColour (theme::accent.withAlpha (0.16f));
        else
            g.setColour (isBlackKey (note) ? theme::panel.darker (0.18f) : theme::panel);
        g.fillRect (row);
        if (chop == proc.selectedSlice)
        {
            g.setColour (theme::accent);
            g.fillRect (row.getX(), row.getY(), 2.0f, row.getHeight());
        }
        row.removeFromLeft (8.0f);

        g.setColour (chop == proc.selectedSlice ? theme::text : theme::textDim);
        g.setFont (theme::mono (12.0f));
        g.drawText (juce::String (chop + 1), row.removeFromLeft (24.0f), juce::Justification::centredLeft);
        g.setColour (theme::textFaint);
        g.setFont (theme::mono (11.5f));
        g.drawText (note >= 0 ? midiNoteName (note) : juce::String ("-"), row.removeFromLeft (34.0f), juce::Justification::centredLeft);

        // Which stem this chop plays, at the end of the row
        const auto tag = stemTag (chop);
        row.setRight (tag.getX() - 4.0f);
        const int own = d.slices[(size_t) chop].settings.stem;
        if (own >= 0)
        {
            g.setColour (theme::inset);
            g.fillRoundedRectangle (tag, 2.0f);
            g.setColour (d.stems != nullptr ? theme::accent : theme::textFaint);
            g.drawRoundedRectangle (tag.reduced (0.5f), 2.0f, 1.0f);
            g.setFont (theme::font (11.5f));
            g.drawText (shortStemName (own), tag, juce::Justification::centred, false);
        }
        else
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.5f));
            g.drawText (shortStemName (d.source), tag, juce::Justification::centred, false);
        }

        const auto& s = d.slices[(size_t) chop];
        const auto lyrics = d.lyricsFor (chop);
        juce::String main = s.settings.label.isNotEmpty() ? s.settings.label : s.info.type;
        g.setColour (s.settings.label.isNotEmpty() ? theme::text : theme::textDim);
        g.setFont (theme::font (12.5f));
        const float mainW = juce::jmin (row.getWidth() * 0.5f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), main) + 8.0f);
        g.drawText (main, row.removeFromLeft (mainW), juce::Justification::centredLeft, true);
        if (lyrics.isNotEmpty())
        {
            g.setColour (theme::warn);
            g.setFont (theme::font (12.0f));
            g.drawText (lyrics, row.reduced (2.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else if (s.info.harmony.isNotEmpty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawText (s.info.harmony, row.reduced (2.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        g.setColour (theme::edge.withAlpha (0.6f));
        g.drawHorizontalLine ((int) (y + rowHeight) - 1, (float) headerArea.getX(), (float) headerArea.getRight());
    }
    g.restoreState();
    g.setColour (theme::edge);
    g.drawVerticalLine (headerArea.getRight() - 1, (float) headerArea.getY(), (float) headerArea.getBottom());
}

void PatternView::drawGrid (juce::Graphics& g)
{
    const auto& d = proc.doc();
    g.setColour (theme::inset);
    g.fillRect (gridArea);

    // Rows shaded like keys, as in FL's piano roll
    for (int chop = 0; chop < (int) d.slices.size(); ++chop)
    {
        const float y = yForChop (chop);
        if (y + rowHeight < (float) gridArea.getY() || y > (float) gridArea.getBottom())
            continue;
        const auto row = juce::Rectangle<float> ((float) gridArea.getX(), y, (float) gridArea.getWidth(), rowHeight);
        g.setColour (isBlackKey (d.noteForSlice (chop)) ? theme::inset : theme::inset.brighter (0.06f));
        g.fillRect (row);
        if (chop == proc.selectedSlice)
        {
            g.setColour (theme::accent.withAlpha (0.06f));
            g.fillRect (row);
        }
        g.setColour (theme::edge.withAlpha (0.5f));
        g.drawHorizontalLine ((int) (y + rowHeight) - 1, row.getX(), row.getRight());
    }

    g.setColour (theme::panel);
    g.fillRect (rulerArea);
    const double bar = barBeats (d.timeSig);
    const double unit = 4.0 / juce::jmax (1, d.timeSig.denominator);
    const double first = juce::jmax (0.0, std::floor (scrollBeats));
    const double last = xToBeat ((float) gridArea.getRight());
    const double step = pxPerBeat * 0.25 >= 7.0 ? 0.25 : pxPerBeat * unit >= 7.0 ? unit : bar;
    g.setFont (theme::mono (11.0f));
    for (double b = std::floor (first / step) * step; b <= last; b += step)
    {
        const float x = beatToX (b);
        if (x < (float) gridArea.getX())
            continue;
        const double inBar = std::fmod (b + 1.0e-9, bar);
        const bool isBar = inBar < 1.0e-6;
        const bool isBeat = std::fmod (b + 1.0e-9, unit) < 1.0e-6;
        g.setColour (isBar ? juce::Colour (0xff4a4f57) : isBeat ? juce::Colour (0xff30343a) : juce::Colour (0xff22252a));
        g.drawVerticalLine ((int) x, (float) gridArea.getY(), (float) gridArea.getBottom());
        g.drawVerticalLine ((int) x, (float) velocityArea.getY(), (float) velocityArea.getBottom());
        if (isBar)
        {
            g.setColour (theme::textDim);
            g.drawText (juce::String ((int) std::round (b / bar) + 1), (int) x + 3, rulerArea.getY(), 40, rulerArea.getHeight(),
                        juce::Justification::centredLeft, false);
            g.drawVerticalLine ((int) x, (float) rulerArea.getY() + 4.0f, (float) rulerArea.getBottom());
        }
        else if (isBeat)
        {
            g.setColour (theme::textFaint);
            g.drawVerticalLine ((int) x, (float) rulerArea.getBottom() - 4.0f, (float) rulerArea.getBottom());
        }
    }

    // Past the end of the pattern
    const float endX = beatToX (working.lengthBeats);
    if (endX < (float) gridArea.getRight())
    {
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (endX, (float) gridArea.getY(), (float) gridArea.getRight() - endX, (float) gridArea.getHeight()));
        g.setColour (theme::accent.withAlpha (0.6f));
        g.drawVerticalLine ((int) endX, (float) rulerArea.getY(), (float) gridArea.getBottom());
    }
}

void PatternView::drawNoteWave (juce::Graphics& g, const PatternNote& n, juce::Rectangle<float> r)
{
    const auto& d = proc.doc();
    const auto& slice = d.slices[(size_t) n.chop];
    const auto source = d.audioFor (slice);
    if (source == nullptr || source->audio.getNumSamples() == 0)
        return;
    const auto& level = peaksFor (source).level;
    const auto how = proc.chopProcessing (n.chop);
    // Played seconds -> source samples, from the note's start offset
    const double perSecond = how.speed * source->sampleRate;
    const double secondsPerBeat = 60.0 / juce::jmax (1.0, proc.getHostBpm());
    const double chopLength = (double) (slice.end - slice.start);

    // Scaled to the chop's own loudest point, like an audio clip in FL's playlist
    float chopPeak = 0.0f;
    for (auto k = slice.start / kPeakBlock; k <= slice.end / kPeakBlock && k < (juce::int64) level.size(); ++k)
        chopPeak = juce::jmax (chopPeak, level[(size_t) k]);
    const float scale = 1.0f / juce::jmax (0.05f, chopPeak);

    const auto area = r.withTrimmedBottom (kTrackHeight).reduced (1.0f, 1.5f);
    const float mid = area.getCentreY(), half = area.getHeight() * 0.5f;
    const float left = juce::jmax (area.getX(), (float) gridArea.getX()), right = juce::jmin (area.getRight(), (float) gridArea.getRight());
    juce::RectangleList<float> bars;
    for (float x = left; x < right; x += 1.0f)
    {
        const double p0 = (n.offset + (xToBeat (x) - n.start) * secondsPerBeat) * perSecond;
        const double p1 = (n.offset + (xToBeat (x + 1.0f) - n.start) * secondsPerBeat) * perSecond;
        if (p0 >= chopLength)
            break;
        double a = juce::jmax (0.0, p0), b = juce::jmin (chopLength, juce::jmax (p0 + 1.0, p1));
        if (how.reverse)
            std::tie (a, b) = std::make_pair (chopLength - b, chopLength - a);
        const int b0 = (int) ((slice.start + (juce::int64) a) / kPeakBlock), b1 = (int) ((slice.start + (juce::int64) b) / kPeakBlock);
        float peak = 0.0f;
        for (int k = juce::jmax (0, b0); k <= b1 && k < (int) level.size(); ++k)
            peak = juce::jmax (peak, level[(size_t) k]);
        const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak * scale) * half);
        bars.addWithoutMerging ({ x, mid - h, 1.0f, h * 2.0f });
    }
    g.fillRectList (bars);
}

void PatternView::drawNotes (juce::Graphics& g)
{
    g.saveState();
    g.reduceClipRegion (gridArea);
    for (size_t i = 0; i < working.notes.size(); ++i)
    {
        const auto& n = working.notes[i];
        if (n.chop < 0 || n.chop >= (int) proc.doc().slices.size())
            continue;
        const auto r = noteBounds (n);
        if (r.getRight() < (float) gridArea.getX() || r.getX() > (float) gridArea.getRight())
            continue;
        const bool sel = i < selected.size() && selected[i];
        auto fill = theme::wave.darker (0.75f * (1.0f - n.velocity));
        if (sel)
            fill = fill.brighter (0.3f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 2.0f);

        g.setColour (juce::Colours::black.withAlpha (0.32f));
        drawNoteWave (g, n, r);

        if (r.getWidth() > 18.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.setFont (theme::font (11.5f));
            g.drawText (noteText (n.chop), r.withTrimmedBottom (kTrackHeight).reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        }

        // Start slider: how far into the chop this note begins
        if (r.getWidth() >= 14.0f)
        {
            const auto track = offsetTrack (r);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (track);
            if (n.offset > 0.0)
            {
                const double length = juce::jmax (1.0e-3, proc.chopPlaySeconds (n.chop));
                const float x = track.getX() + (float) juce::jlimit (0.0, 1.0, n.offset / length) * track.getWidth();
                g.setColour (theme::accent);
                g.fillRect (track.withRight (x));
                g.setColour (theme::text);
                g.fillRect (x - 1.0f, track.getY() - 1.0f, 2.0f, track.getHeight() + 1.0f);
            }
        }

        g.setColour (sel ? theme::text : theme::edge);
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, sel ? 1.5f : 1.0f);

        if (drag == Drag::offset && (int) i == grabbedNote)
        {
            const auto readout = "starts " + juce::String (n.offset, 3) + " s in";
            g.setFont (theme::font (12.0f));
            const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), readout) + 12.0f;
            auto box = juce::Rectangle<float> (r.getX(), r.getY() - 22.0f, w, 18.0f);
            if (box.getY() < (float) gridArea.getY())
                box.setY (r.getBottom() + 4.0f);
            g.setColour (theme::panelRaised);
            g.fillRoundedRectangle (box, 2.0f);
            g.setColour (theme::edge);
            g.drawRoundedRectangle (box.reduced (0.5f), 2.0f, 1.0f);
            g.setColour (theme::text);
            g.drawText (readout, box, juce::Justification::centred, false);
        }
    }

    if (drag == Drag::select && ! selectionBox.isEmpty())
    {
        g.setColour (theme::accent.withAlpha (0.12f));
        g.fillRect (selectionBox);
        g.setColour (theme::accent);
        g.drawRect (selectionBox, 1.0f);
    }

    if (proc.isPatternPlaying())
    {
        g.setColour (theme::text);
        const float x = beatToX (proc.getPatternPosition());
        g.fillRect (x - 0.5f, (float) gridArea.getY(), 1.5f, (float) gridArea.getHeight());
    }
    g.restoreState();
}

void PatternView::drawVelocity (juce::Graphics& g)
{
    g.setColour (theme::inset);
    g.fillRect (velocityArea);
    g.setColour (theme::edge);
    g.drawHorizontalLine (velocityArea.getY(), (float) velocityArea.getX(), (float) velocityArea.getRight());
    g.saveState();
    g.reduceClipRegion (velocityArea);
    const float h = (float) velocityArea.getHeight() - 6.0f;
    for (size_t i = 0; i < working.notes.size(); ++i)
    {
        const auto& n = working.notes[i];
        const float x = beatToX (n.start);
        const float barH = h * n.velocity;
        const bool sel = i < selected.size() && selected[i];
        g.setColour (sel ? theme::accent : theme::wave.withAlpha (0.8f));
        g.fillRect (x, (float) velocityArea.getBottom() - barH - 2.0f, 2.0f, barH);
        g.fillRect (x - 2.0f, (float) velocityArea.getBottom() - barH - 3.0f, 6.0f, 2.0f);
    }
    g.restoreState();
}

void PatternView::paint (juce::Graphics& g)
{
    theme::drawPanel (g, toolbar.toFloat());
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("Length", lengthCaption, juce::Justification::centredLeft);
    g.drawText ("Snap", snapCaption, juce::Justification::centredLeft);

    g.setColour (theme::panel);
    g.fillRect (rulerArea.withLeft (headerArea.getX()).withRight (headerArea.getRight()));
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText ("Chop", rulerArea.withLeft (headerArea.getX() + 8).withWidth (kHeaderWidth - 8), juce::Justification::centredLeft);
    g.drawText ("Stem", rulerArea.withLeft (headerArea.getRight() - 50).withWidth (42), juce::Justification::centred);
    g.drawText ("Velocity", velocityArea.withX (headerArea.getX() + 8).withWidth (kHeaderWidth - 8), juce::Justification::centredLeft);

    if (! proc.doc().hasSample())
    {
        g.setColour (theme::inset);
        g.fillRect (gridArea.getUnion (headerArea));
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawText ("No sample loaded", gridArea.getUnion (headerArea), juce::Justification::centred);
        return;
    }

    drawGrid (g);
    drawRows (g);
    drawNotes (g);
    drawVelocity (g);

    if (working.notes.empty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (13.0f));
        g.drawText ("Empty pattern", gridArea, juce::Justification::centred);
    }
}

//==============================================================================
void PatternView::commit (bool newEdit)
{
    proc.setPattern (working, newEdit);
}

void PatternView::selectOnly (int index)
{
    selected.assign (working.notes.size(), false);
    if (index >= 0 && index < (int) selected.size())
        selected[(size_t) index] = true;
}

void PatternView::deleteSelected()
{
    std::vector<PatternNote> kept;
    for (size_t i = 0; i < working.notes.size(); ++i)
        if (! (i < selected.size() && selected[i]))
            kept.push_back (working.notes[i]);
    if (kept.size() == working.notes.size())
        return;
    working.notes = kept;
    selected.assign (working.notes.size(), false);
    commit (true);
}

void PatternView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.position;
    if (gridArea.contains (p.toInt()))
    {
        const int n = noteAt (p);
        const bool edge = n >= 0 && p.x > noteBounds (working.notes[(size_t) n]).getRight() - 6.0f;
        setMouseCursor (edge || onOffsetTrack (n, p) ? juce::MouseCursor::LeftRightResizeCursor
                        : n >= 0                     ? juce::MouseCursor::DraggingHandCursor
                                                     : juce::MouseCursor::NormalCursor);
    }
    else if (headerArea.contains (p.toInt()))
    {
        const int chop = rowAtY (p.y);
        setMouseCursor (chop >= 0 && stemTag (chop).contains (p) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    else
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }
}

void PatternView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::none;
    dragCommitted = false;
    if (! proc.doc().hasSample())
        return;
    const auto p = e.position;

    // Row header: hear the chop, or pick its stem
    if (headerArea.contains (p.toInt()))
    {
        const int chop = rowAtY (p.y - (float) headerArea.getY() + (float) gridArea.getY());
        if (chop >= 0)
        {
            proc.selectSlice (chop);
            if (e.mods.isPopupMenu() || stemTag (chop).contains (p))
                showStemMenu (chop);
            else
                proc.previewSlice (chop);
        }
        return;
    }

    if (velocityArea.contains (p.toInt()))
    {
        drag = Drag::velocity;
        mouseDrag (e);
        return;
    }

    if (! gridArea.contains (p.toInt()))
        return;

    if (e.mods.isPopupMenu())
    {
        drag = Drag::erase;
        mouseDrag (e);
        return;
    }

    const int hit = noteAt (p);
    if (hit >= 0 && onOffsetTrack (hit, p))
    {
        grabbedNote = hit;
        offsetAtGrab = working.notes[(size_t) hit].offset;
        drag = Drag::offset;
        proc.selectSlice (working.notes[(size_t) hit].chop);
        repaint();
        return;
    }
    if (hit >= 0)
    {
        const auto& n = working.notes[(size_t) hit];
        if (! selected[(size_t) hit])
        {
            if (! e.mods.isShiftDown())
                selected.assign (working.notes.size(), false);
            selected[(size_t) hit] = true;
        }
        grabbedNote = hit;
        drag = p.x > noteBounds (n).getRight() - 6.0f ? Drag::resize : Drag::move;
        proc.selectSlice (n.chop);
        proc.previewSlice (n.chop);
    }
    else if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        drag = Drag::select;
        selectionBox = {};
    }
    else
    {
        const int chop = rowAtY (p.y);
        if (chop < 0)
            return;
        const double start = snapDown (xToBeat (p.x), e.mods.isAltDown());
        if (start < 0.0 || start >= working.lengthBeats)
            return;
        working.notes.push_back ({ chop, start, juce::jmin (defaultLength (chop), working.lengthBeats - start), 0.8f });
        selectOnly ((int) working.notes.size() - 1);
        grabbedNote = (int) working.notes.size() - 1;
        drag = Drag::move;
        commit (true);
        dragCommitted = true; // the new note and dragging it are one undo step
        proc.selectSlice (chop);
        proc.previewSlice (chop);
    }

    grabBeat = xToBeat (p.x);
    grabRow = rowAtY (p.y);
    dragOriginal = working.notes;
    repaint();
}

void PatternView::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.position;
    const bool fine = e.mods.isAltDown();
    switch (drag)
    {
        case Drag::move:
        {
            if (grabbedNote < 0 || e.getDistanceFromDragStart() < 2)
                return;
            const auto& orig = dragOriginal[(size_t) grabbedNote];
            const double newStart = snapNearest (orig.start + (xToBeat (p.x) - grabBeat), fine);
            const double delta = newStart - orig.start;
            const int row = rowAtY (juce::jlimit ((float) gridArea.getY(), (float) gridArea.getBottom() - 1.0f, p.y));
            const int rowDelta = row >= 0 && grabRow >= 0 ? row - grabRow : 0;
            for (size_t i = 0; i < working.notes.size(); ++i)
                if (selected[i])
                {
                    auto& n = working.notes[i];
                    n.start = juce::jlimit (0.0, juce::jmax (0.0, working.lengthBeats - 1.0e-3), dragOriginal[i].start + delta);
                    n.chop = juce::jlimit (0, numRows() - 1, dragOriginal[i].chop + rowDelta);
                }
            if (rowDelta != 0 && working.notes[(size_t) grabbedNote].chop != proc.selectedSlice)
                proc.selectSlice (working.notes[(size_t) grabbedNote].chop);
            commit (! dragCommitted);
            dragCommitted = true;
            break;
        }
        case Drag::resize:
        {
            if (grabbedNote < 0)
                return;
            const auto& orig = dragOriginal[(size_t) grabbedNote];
            const double minLen = snap() > 0.0 && ! fine ? snap() : 1.0 / 64.0;
            const double newEnd = snapNearest (xToBeat (p.x), fine);
            const double delta = juce::jmax (minLen, newEnd - orig.start) - orig.length;
            for (size_t i = 0; i < working.notes.size(); ++i)
                if (selected[i])
                    working.notes[i].length = juce::jmax (minLen, dragOriginal[i].length + delta);
            commit (! dragCommitted);
            dragCommitted = true;
            break;
        }
        case Drag::select:
        {
            selectionBox = juce::Rectangle<float> (e.mouseDownPosition, p).getIntersection (gridArea.toFloat());
            for (size_t i = 0; i < working.notes.size(); ++i)
                selected[i] = noteBounds (working.notes[i]).intersects (selectionBox);
            repaint();
            break;
        }
        case Drag::erase:
        {
            const int hit = noteAt (p);
            if (hit >= 0)
            {
                working.notes.erase (working.notes.begin() + hit);
                selected.erase (selected.begin() + hit);
                commit (! dragCommitted);
                dragCommitted = true;
            }
            break;
        }
        case Drag::velocity:
        {
            const float v = juce::jlimit (0.05f, 1.0f, 1.0f - (p.y - (float) velocityArea.getY() - 3.0f) / ((float) velocityArea.getHeight() - 6.0f));
            bool anySelected = false;
            for (bool s : selected)
                anySelected = anySelected || s;
            bool changed = false;
            for (size_t i = 0; i < working.notes.size(); ++i)
            {
                const float x = beatToX (working.notes[i].start);
                if (std::abs (x - p.x) <= 5.0f && (! anySelected || selected[i]))
                {
                    working.notes[i].velocity = v;
                    changed = true;
                }
            }
            if (changed)
            {
                commit (! dragCommitted);
                dragCommitted = true;
            }
            break;
        }
        case Drag::offset:
        {
            if (grabbedNote < 0 || grabbedNote >= (int) working.notes.size() || e.getDistanceFromDragStart() < 1)
                return;
            auto& n = working.notes[(size_t) grabbedNote];
            // The slider runs the length of the note; its full travel is the whole chop. Shift for fine steps.
            const double length = proc.chopPlaySeconds (n.chop);
            const float width = juce::jmax (12.0f, offsetTrack (noteBounds (n)).getWidth());
            const double moved = (double) (p.x - e.mouseDownPosition.x) / width * length * (e.mods.isShiftDown() ? 0.1 : 1.0);
            n.offset = juce::jlimit (0.0, juce::jmax (0.0, length - 0.005), std::round ((offsetAtGrab + moved) * 1000.0) / 1000.0);
            commit (! dragCommitted);
            dragCommitted = true;
            repaint();
            break;
        }
        case Drag::none:
        default:
            break;
    }
}

void PatternView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int hit = noteAt (e.position);
    if (hit >= 0 && onOffsetTrack (hit, e.position) && working.notes[(size_t) hit].offset > 0.0)
    {
        working.notes[(size_t) hit].offset = 0.0;
        commit (true);
        proc.previewSlice (working.notes[(size_t) hit].chop);
    }
}

juce::String PatternView::getTooltip()
{
    const auto p = getMouseXYRelative().toFloat();
    if (gridArea.contains (p.toInt()))
    {
        const int hit = noteAt (p);
        if (onOffsetTrack (hit, p))
            return "Start: drag right to skip into the chop, for this note only. Shift for fine steps, double-click to reset";
        return {};
    }
    if (headerArea.contains (p.toInt()))
    {
        const int chop = rowAtY (p.y);
        if (chop >= 0 && stemTag (chop).contains (p))
            return "Which stem this chop plays";
        return chop >= 0 ? "Click to hear the chop, right-click to pick its stem" : juce::String();
    }
    return {};
}

void PatternView::mouseUp (const juce::MouseEvent&)
{
    // Let the new start be heard
    if (drag == Drag::offset && dragCommitted && grabbedNote >= 0 && grabbedNote < (int) working.notes.size())
        proc.previewSlice (working.notes[(size_t) grabbedNote].chop, working.notes[(size_t) grabbedNote].offset);
    drag = Drag::none;
    grabbedNote = -1;
    selectionBox = {};
    documentChanged();
}

void PatternView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const double anchor = xToBeat (e.position.x);
        pxPerBeat = juce::jlimit (6.0, 400.0, pxPerBeat * std::exp (w.deltaY * 1.5));
        scrollBeats = anchor - (e.position.x - (float) gridArea.getX()) / pxPerBeat;
    }
    else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        scrollBeats -= d * gridArea.getWidth() / pxPerBeat * 0.4;
    }
    else
    {
        scrollY -= w.deltaY * rowHeight * 8.0f;
    }
    clampScroll();
    updateScrollbars();
    repaint();
}

bool PatternView::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelected();
        return true;
    }
    if (key == juce::KeyPress ('a', cmd, 0))
    {
        selected.assign (working.notes.size(), true);
        repaint();
        return true;
    }
    if (key == juce::KeyPress ('c', cmd, 0) || key == juce::KeyPress ('v', cmd, 0))
    {
        if (key.getKeyCode() == 'c' || key.getKeyCode() == 'C')
        {
            clipboard.clear();
            for (size_t i = 0; i < working.notes.size(); ++i)
                if (selected[i])
                    clipboard.push_back (working.notes[i]);
            return true;
        }
        if (clipboard.empty())
            return true;
        // Paste right after the copied notes, like duplicating them
        double first = 1.0e9, last = 0.0;
        for (const auto& n : clipboard)
        {
            first = std::min (first, n.start);
            last = std::max (last, n.end());
        }
        const double s = snap() > 0.0 ? snap() : 0.25;
        const double offset = std::ceil ((last - first) / s - 1.0e-9) * s;
        selected.assign (working.notes.size(), false);
        double furthest = working.lengthBeats;
        for (auto n : clipboard)
        {
            n.start += offset;
            working.notes.push_back (n);
            selected.push_back (true);
            furthest = std::max (furthest, n.end());
        }
        // Grow the pattern (in whole bars) so pasted notes aren't lost off the end
        const double bar = barBeats (proc.doc().timeSig);
        working.lengthBeats = std::ceil (furthest / bar - 1.0e-9) * bar;
        clipboard.clear();
        for (size_t i = 0; i < working.notes.size(); ++i)
            if (selected[i])
                clipboard.push_back (working.notes[i]);
        commit (true);
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::leftKey)
        || key.isKeyCode (juce::KeyPress::rightKey))
    {
        bool any = false;
        const int dRow = key.isKeyCode (juce::KeyPress::upKey) ? 1 : key.isKeyCode (juce::KeyPress::downKey) ? -1 : 0;
        const double s = snap() > 0.0 ? snap() : 0.25;
        const double dBeat = key.isKeyCode (juce::KeyPress::rightKey) ? s : key.isKeyCode (juce::KeyPress::leftKey) ? -s : 0.0;
        for (size_t i = 0; i < working.notes.size(); ++i)
            if (selected[i])
            {
                auto& n = working.notes[i];
                n.chop = juce::jlimit (0, numRows() - 1, n.chop + dRow);
                n.start = juce::jlimit (0.0, working.lengthBeats - 1.0e-3, n.start + dBeat);
                any = true;
            }
        if (any)
            commit (true);
        return any;
    }
    return false;
}

} // namespace choplab
