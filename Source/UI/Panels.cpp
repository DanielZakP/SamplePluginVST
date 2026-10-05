#include "Panels.h"

namespace choplab
{

namespace
{
juce::String semitones (double v)
{
    const auto r = std::round (v);
    return (r > 0 ? "+" : "") + juce::String ((int) r) + " st";
}
juce::String ratio (double v) { return juce::String (v, 2) + "x"; }
juce::String decibels (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String millis (double v) { return v < 10.0 ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
} // namespace

//==============================================================================
SliceInspector::SliceInspector (ChopLabProcessor& p, std::function<juce::File (int)> exporter)
    : proc (p),
      exportChop (std::move (exporter)),
      dragWav ("Drag WAV", [this] { return exportChop (shown); }),
      pitch ("PITCH", -24.0, 24.0, 0.0, 1.0, semitones),
      speed ("SPEED", 0.25, 4.0, 1.0, 0.01, ratio, 1.0),
      gain ("GAIN", -24.0, 12.0, 0.0, 0.1, decibels),
      attack ("FADE IN", 0.0, 500.0, 0.0, 0.1, millis, 40.0),
      release ("RELEASE", 1.0, 2000.0, 15.0, 0.1, millis, 120.0)
{
    label.setFont (theme::font (15.0f));
    label.setTextToShowWhenEmpty ("Label (e.g. kick, vox)", theme::textFaint);
    label.setIndents (8, 6);
    label.setSelectAllWhenFocused (true);
    label.onReturnKey = [this] { push(); getParentComponent()->grabKeyboardFocus(); };
    label.onFocusLost = [this] { push(); };
    label.onEscapeKey = [this]
    {
        const auto& d = proc.doc();
        if (showingValidChop())
            label.setText (d.slices[(size_t) shown].settings.label, juce::dontSendNotification);
        getParentComponent()->grabKeyboardFocus();
    };
    addAndMakeVisible (label);

    lyrics.setFont (theme::font (15.0f));
    lyrics.setTextToShowWhenEmpty ("Lyrics", theme::textFaint);
    lyrics.setIndents (8, 6);
    lyrics.setSelectAllWhenFocused (true);
    lyrics.setTooltip ("Words in this chop. Filled in by Find lyrics; type here to fix them");
    lyrics.onReturnKey = [this] { pushLyrics(); getParentComponent()->grabKeyboardFocus(); };
    lyrics.onFocusLost = [this] { pushLyrics(); };
    lyrics.onEscapeKey = [this]
    {
        if (showingValidChop())
            lyrics.setText (proc.doc().lyricsFor (shown), juce::dontSendNotification);
        getParentComponent()->grabKeyboardFocus();
    };
    addAndMakeVisible (lyrics);

    for (auto* k : { &pitch, &speed, &gain, &attack, &release })
    {
        k->onChange = [this] (double) { push(); };
        addAndMakeVisible (*k);
    }
    pitch.slider.setTooltip ("Pitch shift in semitones, without changing the length");
    speed.slider.setTooltip ("Playback speed. With 'Keep pitch' on this only changes the length (time-stretch); off, it's tape-style "
                             "and the pitch moves with it");
    attack.slider.setTooltip ("Fade-in time when the chop starts");
    release.slider.setTooltip ("Fade-out time after you let go of the key (Gate mode)");

    for (auto* t : { &keepPitch, &reverse })
    {
        t->onClick = [this] { push(); };
        addAndMakeVisible (*t);
    }
    keepPitch.setTooltip ("On: speed changes length only. Off: tape-style, faster = higher");

    playButton.onClick = [this] { proc.previewSlice (shown); };
    barOneButton.setTooltip ("Make this chop the start of bar 1; all beat positions are counted from here");
    barOneButton.onClick = [this]
    {
        const auto& d = proc.doc();
        if (showingValidChop())
            proc.setDownbeat (d.secondsAt (d.slices[(size_t) shown].start));
    };
    resetButton.setTooltip ("Reset pitch, speed, gain and fades for this chop (keeps the label)");
    resetButton.onClick = [this]
    {
        if (! showingValidChop())
            return;
        SliceSettings fresh;
        fresh.label = proc.doc().slices[(size_t) shown].settings.label;
        proc.setSliceSettings (shown, fresh);
    };
    dragWav.setTooltip ("Drag this chop (with its pitch/speed/reverse) into FL's playlist or channel rack. Click to just save it");
    dragWav.onClick = [this] { exportChop (shown); };
    for (auto* b : { &playButton, &barOneButton, &resetButton })
        addAndMakeVisible (*b);
    addAndMakeVisible (dragWav);
}

bool SliceInspector::showingValidChop() const
{
    return shown >= 0 && shown < (int) proc.doc().slices.size();
}

SliceSettings SliceInspector::current() const
{
    const auto& d = proc.doc();
    SliceSettings s;
    if (showingValidChop())
        s = d.slices[(size_t) shown].settings;
    s.label = label.getText().trim();
    s.pitch = (float) pitch.getValue();
    s.speed = (float) speed.getValue();
    s.gainDb = (float) gain.getValue();
    s.attackMs = (float) attack.getValue();
    s.releaseMs = (float) release.getValue();
    s.keepPitch = keepPitch.getToggleState();
    s.reverse = reverse.getToggleState();
    return s;
}

// Edits always go to the chop the panel is showing, even if the selection has just moved on
// (e.g. clicking another chop while a label is half typed).
void SliceInspector::push()
{
    const auto& d = proc.doc();
    const int i = shown;
    if (! showingValidChop())
        return;
    const auto s = current();
    const auto& old = d.slices[(size_t) i].settings;
    using juce::exactlyEqual;
    if (s.label == old.label && exactlyEqual (s.pitch, old.pitch) && exactlyEqual (s.speed, old.speed) && exactlyEqual (s.gainDb, old.gainDb)
        && exactlyEqual (s.attackMs, old.attackMs) && exactlyEqual (s.releaseMs, old.releaseMs) && s.keepPitch == old.keepPitch
        && s.reverse == old.reverse)
        return;
    proc.setSliceSettings (i, s);
}

void SliceInspector::pushLyrics()
{
    if (showingValidChop() && lyrics.getText().trim() != proc.doc().lyricsFor (shown))
        proc.setSliceLyrics (shown, lyrics.getText());
}

void SliceInspector::documentChanged()
{
    const auto& d = proc.doc();
    const int i = proc.selectedSlice;
    if (i != shown && label.hasKeyboardFocus (false))
        push(); // keep a label that was typed but not confirmed
    if (i != shown && lyrics.hasKeyboardFocus (false))
        pushLyrics();
    const bool valid = i >= 0 && i < (int) d.slices.size();

    for (auto* c : std::initializer_list<juce::Component*> { &label, &lyrics, &pitch, &speed, &gain, &attack, &release, &keepPitch, &reverse,
                                                             &playButton, &barOneButton, &resetButton, &dragWav })
        c->setEnabled (valid);

    if (valid)
    {
        const auto& s = d.slices[(size_t) i].settings;
        if (! label.hasKeyboardFocus (false) || i != shown)
            label.setText (s.label, juce::dontSendNotification);
        if (! lyrics.hasKeyboardFocus (false) || i != shown)
        {
            lyrics.setText (d.lyricsFor (i), juce::dontSendNotification);
            lyrics.applyColourToAllText (s.lyricsEdited ? theme::text : theme::textDim);
        }
        pitch.setValue (s.pitch);
        speed.setValue (s.speed);
        gain.setValue (s.gainDb);
        attack.setValue (s.attackMs);
        release.setValue (s.releaseMs);
        keepPitch.setToggleState (s.keepPitch, juce::dontSendNotification);
        reverse.setToggleState (s.reverse, juce::dontSendNotification);
    }
    else
    {
        label.setText ({}, juce::dontSendNotification);
        lyrics.setText ({}, juce::dontSendNotification);
    }
    shown = i;
    repaint();
}

void SliceInspector::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    const auto& d = proc.doc();
    const int i = proc.selectedSlice;
    auto h = headerArea;
    if (i < 0 || i >= (int) d.slices.size())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("No chop selected", h, juce::Justification::centredLeft);
        return;
    }

    const auto& s = d.slices[(size_t) i];
    const auto colour = theme::sliceColour (i);
    g.setColour (colour);
    g.fillRoundedRectangle (h.removeFromLeft (6).toFloat().reduced (0, 4), 2.0f);
    h.removeFromLeft (8);
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    const juce::String title = "Chop " + juce::String (i + 1);
    g.drawText (title, h, juce::Justification::centredLeft);
    const int titleW = juce::GlyphArrangement::getStringWidthInt (theme::font (17.0f, true), title) + 10;
    const int note = d.noteForSlice (i);
    if (note >= 0)
    {
        auto pill = h.withTrimmedLeft (titleW).withWidth (46).reduced (0, 6).toFloat();
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (pill, 4.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::mono (12.5f));
        g.drawText (midiNoteName (note), pill, juce::Justification::centred);
    }

    // Where it sits and what it is
    const auto pos = d.gridPosition (s.start);
    juce::String where = pos.bar < 1 ? juce::String ("Pickup (before bar 1)")
                                     : "Bar " + juce::String (pos.bar) + ", beat " + juce::String (pos.beat) + " of "
                                           + juce::String (d.timeSig.numerator);
    if (pos.bar >= 1 && pos.sixteenth > 1)
        where << " (" << pos.describe << ")";
    if (! pos.onGrid)
        where << ", off grid";

    const double beats = d.lengthInBeats (s);
    juce::String what = juce::String (beats, 2) + " beats, " + juce::String (d.secondsAt (s.end - s.start), 2) + " s";
    if (s.info.type.isNotEmpty())
        what << "  |  " << s.info.type;
    if (s.info.harmony.isNotEmpty())
        what << "  |  " << s.info.harmony;

    auto info = infoArea;
    g.setFont (theme::font (13.5f));
    g.setColour (pos.describe == "Bar start" ? theme::accent : theme::text);
    g.drawText (where, info.removeFromTop (18), juce::Justification::centredLeft, true);
    g.setColour (theme::textDim);
    g.drawText (what, info.removeFromTop (18), juce::Justification::centredLeft, true);
}

void SliceInspector::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (30);
    dragWav.setBounds (top.removeFromRight (104).reduced (0, 2));
    top.removeFromRight (6);
    playButton.setBounds (top.removeFromRight (56).reduced (0, 2));
    headerArea = top;

    r.removeFromTop (6);
    auto fields = r.removeFromTop (30);
    label.setBounds (fields.removeFromLeft (fields.getWidth() * 2 / 5));
    fields.removeFromLeft (6);
    lyrics.setBounds (fields);
    r.removeFromTop (6);
    infoArea = r.removeFromTop (38);
    r.removeFromTop (4);

    auto knobs = r.removeFromTop (88);
    const int w = knobs.getWidth() / 5;
    for (auto* k : { &pitch, &speed, &gain, &attack, &release })
        k->setBounds (knobs.removeFromLeft (w).reduced (2, 0));

    r.removeFromTop (8);
    auto row = r.removeFromTop (26);
    keepPitch.setBounds (row.removeFromLeft (110));
    reverse.setBounds (row.removeFromLeft (96));
    resetButton.setBounds (row.removeFromRight (60));
    row.removeFromRight (6);
    barOneButton.setBounds (row.removeFromRight (98));
}

//==============================================================================
GlobalPanel::GlobalPanel (ChopLabProcessor& p)
    : proc (p),
      pitch ("PITCH", -24.0, 24.0, 0.0, 1.0, semitones),
      speed ("SPEED", 0.25, 4.0, 1.0, 0.01, ratio, 1.0),
      volume ("VOLUME", -24.0, 12.0, 0.0, 0.1, decibels),
      dragFull ("Drag sample", [this] { return proc.renderFullToFile (proc.getDragFolder()); })
{
    for (auto* k : { &pitch, &speed, &volume })
    {
        k->onChange = [this] (double) { push(); };
        addAndMakeVisible (*k);
    }
    pitch.slider.setTooltip ("Pitch shift for every chop (adds to each chop's own pitch)");
    speed.slider.setTooltip ("Speed for every chop (multiplies each chop's own speed)");

    for (auto* t : { &keepPitch, &sync, &mono })
    {
        t->onClick = [this] { push(); };
        addAndMakeVisible (*t);
    }
    keepPitch.setTooltip ("On: speed changes length only. Off: tape-style, faster = higher");
    sync.setTooltip ("Time-stretch everything so the sample's tempo follows FL's project tempo");
    mono.setTooltip ("Each new chop cuts off whatever is still playing (like an MPC mute group)");

    reverseSample.setTooltip ("Flip the whole sample backwards. Chop markers, labels and settings are mirrored with it");
    reverseSample.onClick = [this] { proc.setReversed (reverseSample.getToggleState()); };
    addAndMakeVisible (reverseSample);

    for (int n = 0; n < 128; ++n)
        rootNote.addItem (midiNoteName (n), n + 1);
    rootNote.setTooltip ("The note that plays chop 1. Chop 2 is the next note up, and so on");
    rootNote.onChange = [this] { push(); };
    addAndMakeVisible (rootNote);

    trigger.addItem ("Gate (note length)", 1);
    trigger.addItem ("One-shot (whole chop)", 2);
    trigger.setTooltip ("Gate: the chop stops when the note ends. One-shot: it always plays to the end");
    trigger.onChange = [this] { push(); };
    addAndMakeVisible (trigger);

    dragFull.setTooltip ("Drag the whole sample with these global settings into FL. Click to just save it");
    addAndMakeVisible (dragFull);
}

void GlobalPanel::push()
{
    auto g = proc.doc().global;
    g.pitch = (float) pitch.getValue();
    g.speed = (float) speed.getValue();
    g.gainDb = (float) volume.getValue();
    g.keepPitch = keepPitch.getToggleState();
    g.syncToHost = sync.getToggleState();
    g.mono = mono.getToggleState();
    g.rootNote = juce::jlimit (0, 127, rootNote.getSelectedId() - 1);
    g.oneShot = trigger.getSelectedId() == 2;
    proc.setGlobalSettings (g);
}

void GlobalPanel::documentChanged()
{
    const auto& d = proc.doc();
    const auto& g = d.global;
    pitch.setValue (g.pitch);
    speed.setValue (g.speed);
    volume.setValue (g.gainDb);
    keepPitch.setToggleState (g.keepPitch, juce::dontSendNotification);
    sync.setToggleState (g.syncToHost, juce::dontSendNotification);
    mono.setToggleState (g.mono, juce::dontSendNotification);
    reverseSample.setToggleState (d.reversed, juce::dontSendNotification);
    reverseSample.setEnabled (d.hasSample() && ! proc.isAnalysing());
    dragFull.setEnabled (d.hasSample());
    rootNote.setSelectedId (g.rootNote + 1, juce::dontSendNotification);
    trigger.setSelectedId (g.oneShot ? 2 : 1, juce::dontSendNotification);
    repaint();
}

void GlobalPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("Whole sample", captionArea, juce::Justification::centredLeft);

    const auto& d = proc.doc();
    if (d.global.syncToHost && d.hasSample())
    {
        const double ratio = proc.getHostBpm() / juce::jmax (1.0, d.bpm);
        g.setColour (theme::good);
        g.setFont (theme::font (12.5f));
        g.drawText (juce::String (d.bpm, 1) + " to " + juce::String (proc.getHostBpm(), 1) + " BPM (" + juce::String (ratio, 2) + "x)",
                    captionArea.withTrimmedLeft (110), juce::Justification::centredLeft, true);
    }

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("CHOP 1 ON", rootCaption, juce::Justification::centredLeft);
    g.drawText ("NOTE LENGTH", triggerCaption, juce::Justification::centredLeft);
}

void GlobalPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (30);
    dragFull.setBounds (top.removeFromRight (116).reduced (0, 2));
    captionArea = top;
    r.removeFromTop (4);

    auto row = r.removeFromTop (88);
    auto knobs = row.removeFromLeft (row.getWidth() * 3 / 5);
    const int w = knobs.getWidth() / 3;
    for (auto* k : { &pitch, &speed, &volume })
        k->setBounds (knobs.removeFromLeft (w).reduced (2, 0));
    row.removeFromLeft (8);
    keepPitch.setBounds (row.removeFromTop (26));
    reverseSample.setBounds (row.removeFromTop (26));

    r.removeFromTop (6);
    sync.setBounds (r.removeFromTop (24));
    mono.setBounds (r.removeFromTop (24));
    r.removeFromTop (6);

    auto captions = r.removeFromTop (14);
    rootCaption = captions.removeFromLeft (90);
    captions.removeFromLeft (8);
    triggerCaption = captions;
    auto combos = r.removeFromTop (26);
    rootNote.setBounds (combos.removeFromLeft (90));
    combos.removeFromLeft (8);
    trigger.setBounds (combos);
}

} // namespace choplab
