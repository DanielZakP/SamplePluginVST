#include "PluginEditor.h"

using namespace choplab;

namespace
{
struct GridChoice
{
    const char* name;
    double beats;
};
const GridChoice kGridChoices[] { { "1 bar", 0.0 },          { "1/2 note", 2.0 },          { "1/4 (beat)", 1.0 },
                                  { "1/8", 0.5 },            { "1/16", 0.25 },             { "1/8 triplet", 1.0 / 3.0 },
                                  { "1/16 triplet", 1.0 / 6.0 } };

const TimeSignature kTimeSigs[] { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } };

juce::String formatBpm (double bpm)
{
    return std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String ((int) std::round (bpm)) : juce::String (bpm, 2);
}
} // namespace

ChopLabEditor::ChopLabEditor (ChopLabProcessor& p)
    : AudioProcessorEditor (p),
      proc (p),
      waveform (p),
      midiDrag ("Drag MIDI pattern", [this] { return proc.writeMidiPattern (proc.getDragFolder()); }),
      table (p),
      inspector (p, [this] (int i) { return exportChop (i); }),
      globalPanel (p),
      lyricsCard (p),
      patternView (p)
{
    setWantsKeyboardFocus (true);

    undoButton.onClick = [this] { proc.undo(); };
    redoButton.onClick = [this] { proc.redo(); };
    undoButton.setTooltip ("Undo (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y)");
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);

    loadButton.onClick = [this] { chooseFile(); };
    loadButton.setTooltip ("Or just drag an audio file onto the plugin");
    addAndMakeVisible (loadButton);

    bpmValue.setEditable (false, true, false);
    bpmValue.setFont (theme::font (26.0f, true));
    bpmValue.setJustificationType (juce::Justification::centredLeft);
    bpmValue.setTooltip ("Double-click to type the tempo");
    bpmValue.onTextChange = [this]
    {
        const double v = bpmValue.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (v >= 20.0 && v <= 400.0)
            proc.setBpm (v);
        else
            refresh();
    };
    addAndMakeVisible (bpmValue);

    halfButton.setTooltip ("Half the tempo (if it was read twice as fast as it feels)");
    doubleButton.setTooltip ("Double the tempo (if it was read half as fast as it feels)");
    halfButton.onClick = [this] { proc.setBpm (proc.doc().bpm * 0.5); };
    doubleButton.onClick = [this] { proc.setBpm (proc.doc().bpm * 2.0); };
    addAndMakeVisible (halfButton);
    addAndMakeVisible (doubleButton);

    timeSigBox.addItem ("Auto", 1);
    for (int i = 0; i < (int) std::size (kTimeSigs); ++i)
        timeSigBox.addItem (kTimeSigs[i].toString(), i + 2);
    timeSigBox.setTooltip ("Auto picks between 3/4 and 4/4. Anything else, set it here");
    timeSigBox.onChange = [this]
    {
        const int id = timeSigBox.getSelectedId();
        if (id == 1)
            proc.setTimeSignature (proc.doc().timeSig, true);
        else if (id >= 2)
            proc.setTimeSignature (kTimeSigs[id - 2], false);
    };
    addAndMakeVisible (timeSigBox);

    addAndMakeVisible (waveform);
    waveform.onChopClicked = [this] (int i) { selectAndPlay (i); };
    waveform.onChopDraggedOut = [this] (int i)
    {
        const auto f = exportChop (i);
        if (f.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, &waveform);
    };

    for (auto* b : { &modeTransients, &modeGrid, &modeManual })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    modeTransients.setTooltip ("Chop at every hit (drums, plucks, vocal syllables)");
    modeGrid.setTooltip ("Chop on the beat grid, e.g. every 1/8 note");
    modeManual.setTooltip ("Place every chop yourself: double-click the waveform to add, right-click to remove");
    modeTransients.onClick = [this] { setMode (ChopMode::transients); };
    modeGrid.onClick = [this] { setMode (ChopMode::grid); };
    modeManual.onClick = [this] { setMode (ChopMode::manual); };

    sensitivity.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    sensitivity.setRange (0.0, 1.0, 0.01);
    sensitivity.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    sensitivity.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
    sensitivity.setTooltip ("Higher = more chops (quieter hits count too)");
    sensitivity.onValueChange = [this]
    {
        auto c = proc.doc().chop;
        c.sensitivity = (float) sensitivity.getValue();
        proc.setChopSettings (c, true);
    };
    addAndMakeVisible (sensitivity);

    minGap.setSliderStyle (juce::Slider::LinearHorizontal);
    minGap.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    minGap.setRange (10.0, 1000.0, 1.0);
    minGap.setSkewFactorFromMidPoint (120.0);
    minGap.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)) + " ms"; };
    minGap.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue(); };
    minGap.setTooltip ("Shortest allowed chop. Raise it to stop flams and ghost notes making tiny chops");
    minGap.onValueChange = [this]
    {
        auto c = proc.doc().chop;
        c.minLengthMs = (float) minGap.getValue();
        proc.setChopSettings (c, true);
    };
    addAndMakeVisible (minGap);

    for (int i = 0; i < (int) std::size (kGridChoices); ++i)
        gridBox.addItem (kGridChoices[i].name, i + 1);
    gridBox.onChange = [this]
    {
        auto c = proc.doc().chop;
        c.gridBeats = kGridChoices[juce::jlimit (0, (int) std::size (kGridChoices) - 1, gridBox.getSelectedId() - 1)].beats;
        proc.setChopSettings (c, true);
    };
    addAndMakeVisible (gridBox);

    playAllButton.onClick = [this] { proc.previewFull(); };
    playAllButton.setTooltip ("Play the whole sample with the global settings (Space plays the selected chop)");
    stopButton.onClick = [this] { proc.stopPreview(); };
    exportButton.onClick = [this] { exportAll(); };
    exportButton.setTooltip ("Save every chop as its own WAV file");
    midiDrag.setTooltip ("Drag into FL's piano roll or playlist: a MIDI pattern that plays the chops in their original order");
    midiDrag.onClick = [this]
    {
        const auto f = proc.writeMidiPattern (proc.getDragFolder());
        setStatus (f.existsAsFile() ? "Saved " + f.getFullPathName() : juce::String ("Couldn't write the MIDI file"), ! f.existsAsFile());
    };
    for (auto* c : std::initializer_list<juce::Component*> { &playAllButton, &stopButton, &exportButton, &midiDrag })
        addAndMakeVisible (c);

    addAndMakeVisible (table);
    table.onChopClicked = [this] (int i) { selectAndPlay (i); };
    addAndMakeVisible (inspector);
    addAndMakeVisible (globalPanel);
    addAndMakeVisible (lyricsCard);
    addChildComponent (patternView);

    for (auto* tab : { &chopsTab, &rollTab })
    {
        tab->setClickingTogglesState (false);
        tab->setConnectedEdges (tab == &chopsTab ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
        addAndMakeVisible (*tab);
    }
    chopsTab.setTooltip ("Waveform, chop list and chop settings");
    rollTab.setTooltip ("Piano roll where every row is a chop, with its label and lyrics. Drag the result into FL");
    chopsTab.onClick = [this] { showPage (0); };
    rollTab.onClick = [this] { showPage (1); };

    // Last, so every child (including slider text boxes created in member constructors) picks it up.
    setLookAndFeel (&lnf);

    proc.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (1180, 740, 2600, 1700);
    setSize (1360, 820);
    showPage (proc.currentPage);
    refresh();
    startTimerHz (8);
}

ChopLabEditor::~ChopLabEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void ChopLabEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void ChopLabEditor::refresh()
{
    const auto& d = proc.doc();
    const bool has = d.hasSample();

    if (! bpmValue.isBeingEdited())
        bpmValue.setText (has ? formatBpm (d.bpm) : juce::String ("--"), juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &bpmValue, &halfButton, &doubleButton, &timeSigBox, &playAllButton,
                                                             &stopButton, &exportButton, &midiDrag })
        c->setEnabled (has);

    undoButton.setEnabled (proc.canUndo());
    redoButton.setEnabled (proc.canRedo());
    timeSigBox.changeItemText (1, "Auto (" + d.timeSig.toString() + ")");
    int tsId = 1;
    if (! d.meterIsAuto)
        for (int i = 0; i < (int) std::size (kTimeSigs); ++i)
            if (kTimeSigs[i] == d.timeSig)
                tsId = i + 2;
    timeSigBox.setSelectedId (tsId, juce::dontSendNotification);

    const auto mode = d.chop.mode;
    modeTransients.setToggleState (mode == ChopMode::transients, juce::dontSendNotification);
    modeGrid.setToggleState (mode == ChopMode::grid, juce::dontSendNotification);
    modeManual.setToggleState (mode == ChopMode::manual, juce::dontSendNotification);
    const bool chopsPage = proc.currentPage == 0;
    sensitivity.setVisible (chopsPage && mode == ChopMode::transients);
    minGap.setVisible (chopsPage && mode == ChopMode::transients);
    gridBox.setVisible (chopsPage && mode == ChopMode::grid);
    sensitivity.setValue (d.chop.sensitivity, juce::dontSendNotification);
    minGap.setValue (d.chop.minLengthMs, juce::dontSendNotification);
    int gridId = 3;
    for (int i = 0; i < (int) std::size (kGridChoices); ++i)
        if (std::abs (kGridChoices[i].beats - d.chop.gridBeats) < 1.0e-6)
            gridId = i + 1;
    gridBox.setSelectedId (gridId, juce::dontSendNotification);

    if (proc.isAnalysing())
        setStatus ("Analyzing...");
    else if (proc.getError().isNotEmpty())
        setStatus (proc.getError(), true);
    else if (status == "Analyzing...")
        setStatus ({});

    waveform.documentChanged();
    table.documentChanged();
    inspector.documentChanged();
    globalPanel.documentChanged();
    lyricsCard.documentChanged();
    patternView.documentChanged();
    repaint();
}

void ChopLabEditor::showPage (int page)
{
    proc.currentPage = page;
    const bool chops = page == 0;
    for (auto* c : std::initializer_list<juce::Component*> { &waveform, &modeTransients, &modeGrid, &modeManual, &playAllButton,
                                                             &stopButton, &exportButton, &midiDrag, &table, &inspector, &globalPanel })
        c->setVisible (chops);
    const auto mode = proc.doc().chop.mode;
    sensitivity.setVisible (chops && mode == ChopMode::transients);
    minGap.setVisible (chops && mode == ChopMode::transients);
    gridBox.setVisible (chops && mode == ChopMode::grid);
    patternView.setVisible (! chops);
    chopsTab.setToggleState (chops, juce::dontSendNotification);
    rollTab.setToggleState (! chops, juce::dontSendNotification);
    if (! chops)
        patternView.grabKeyboardFocus();
    repaint();
}

void ChopLabEditor::timerCallback()
{
    if (std::abs (proc.getHostBpm() - shownHostBpm) > 0.001)
    {
        shownHostBpm = proc.getHostBpm();
        repaint (projectCard);
        globalPanel.repaint();
    }
    if (status.isNotEmpty() && ! statusIsError && ! proc.isAnalysing() && juce::Time::getMillisecondCounter() - statusTime > 7000)
        setStatus ({});
}

void ChopLabEditor::setStatus (const juce::String& s, bool isError)
{
    status = s;
    statusIsError = isError;
    statusTime = juce::Time::getMillisecondCounter();
    repaint (header);
}

//==============================================================================
void ChopLabEditor::setMode (ChopMode m)
{
    auto c = proc.doc().chop;
    c.mode = m;
    proc.setChopSettings (c, m != ChopMode::manual);
}

void ChopLabEditor::selectAndPlay (int index)
{
    proc.selectSlice (index);
    proc.previewSlice (index);
}

void ChopLabEditor::chopClicked (int index)
{
    selectAndPlay (index);
}

juce::File ChopLabEditor::exportChop (int index)
{
    const auto f = proc.renderSliceToFile (index, proc.getDragFolder());
    setStatus (f.existsAsFile() ? "Saved " + f.getFullPathName() : juce::String ("Couldn't save the chop"), ! f.existsAsFile());
    return f;
}

void ChopLabEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                                  proc.loadFile (f);
                          });
}

void ChopLabEditor::exportAll()
{
    chooser = std::make_unique<juce::FileChooser> ("Export chops to folder", proc.getDragFolder());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto dir = fc.getResult();
                              if (! dir.isDirectory())
                                  return;
                              const int n = proc.exportAllSlices (dir);
                              setStatus ("Exported " + juce::String (n) + " chops to " + dir.getFullPathName(), n == 0);
                          });
}

//==============================================================================
bool ChopLabEditor::keyPressed (const juce::KeyPress& key)
{
    const auto& d = proc.doc();
    const int sel = proc.selectedSlice;
    const int count = (int) d.slices.size();

    if (key == juce::KeyPress::spaceKey && proc.currentPage == 1)
    {
        proc.playPattern (! proc.isPatternPlaying());
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        if (sel >= 0)
            proc.previewSlice (sel);
        else
            proc.previewFull();
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        proc.stopPreview();
        return true;
    }
    if ((key == juce::KeyPress::rightKey || key == juce::KeyPress::leftKey) && count > 0)
    {
        const int next = juce::jlimit (0, count - 1, sel + (key == juce::KeyPress::rightKey ? 1 : -1));
        selectAndPlay (next);
        return true;
    }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && sel > 0)
    {
        proc.removeMarker (sel);
        return true;
    }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0))
    {
        chooseFile();
        return true;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        proc.undo();
        return true;
    }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)
        || key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        proc.redo();
        return true;
    }
    return false;
}

bool ChopLabEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (ChopLabProcessor::canLoad (juce::File (f)))
            return true;
    return false;
}

void ChopLabEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragHover = true;
    repaint();
}

void ChopLabEditor::fileDragExit (const juce::StringArray&)
{
    fileDragHover = false;
    repaint();
}

void ChopLabEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragHover = false;
    for (const auto& f : files)
        if (ChopLabProcessor::canLoad (juce::File (f)))
        {
            proc.loadFile (juce::File (f));
            break;
        }
    repaint();
}

//==============================================================================
void ChopLabEditor::paintCards (juce::Graphics& g)
{
    const auto& d = proc.doc();
    const bool has = d.hasSample();

    for (auto r : { tempoCard, keyCard, timeCard, projectCard })
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
    }

    auto sub = [&] (juce::Rectangle<int> card, const juce::String& s, juce::Colour c = theme::textDim)
    {
        g.setColour (c);
        g.setFont (theme::font (12.5f));
        g.drawText (s, card.reduced (12, 0).removeFromBottom (24).withTrimmedBottom (6), juce::Justification::centredLeft, true);
    };
    auto big = [&] (juce::Rectangle<int> area, const juce::String& s, juce::Colour c = theme::text)
    {
        g.setColour (c);
        g.setFont (theme::font (24.0f, true));
        g.drawText (s, area, juce::Justification::centredLeft, true);
    };

    // Tempo
    drawCaption (g, tempoCard.reduced (12, 8), "Tempo");
    g.setColour (theme::textDim);
    g.setFont (theme::font (13.0f));
    g.drawText ("BPM", bpmValue.getBounds().translated (juce::GlyphArrangement::getStringWidthInt (bpmValue.getFont(), bpmValue.getText()) + 12, 2),
                juce::Justification::centredLeft);
    if (has)
    {
        const auto& t = d.detectedTempo;
        juce::String s = std::abs (d.bpm - t.bpm) < 0.001 ? "Detected" : "Detected " + formatBpm (t.bpm) + ", set by you";
        if (t.loopDetected && std::abs (d.bpm - t.bpm) < 0.001)
            s << ", sample is a " << juce::String ((int) std::lround (d.sample->lengthSeconds() * d.bpm / 60.0 / d.timeSig.quarterBeatsPerBar()))
              << "-bar loop";
        sub (tempoCard, s);
    }

    // Key
    drawCaption (g, keyCard.reduced (12, 8), "Key");
    auto keyBig = keyCard.reduced (12, 0).withTrimmedTop (22).withHeight (30);
    if (! has)
        big (keyBig, "--", theme::textFaint);
    else if (d.key.hasKey)
    {
        big (keyBig, keyName (d.key.best));
        sub (keyCard, "or " + keyName (d.key.alt) + "  |  " + juce::String (juce::roundToInt (d.key.confidence * 100.0f)) + "% sure");
    }
    else
    {
        big (keyBig, "No clear key", theme::textDim);
        sub (keyCard, "Mostly drums or noise");
    }

    // Time signature
    drawCaption (g, timeCard.reduced (12, 8), "Time signature");
    if (has)
    {
        juce::String s;
        if (! d.meterIsAuto)
            s = "Set by you";
        else
            s = d.detectedGrid.meterConfidence > 0.6f ? "Detected, fairly sure" : "Detected, best guess";
        const auto first = d.gridPosition (0);
        if (d.downbeatSeconds > 0.01)
            s << "  |  bar 1 at " << juce::String (d.downbeatSeconds, 2) << " s";
        juce::ignoreUnused (first);
        sub (timeCard, s);
    }

    // Project
    drawCaption (g, projectCard.reduced (12, 8), "Project (FL Studio)");
    big (projectCard.reduced (12, 0).withTrimmedTop (22).withHeight (30), formatBpm (proc.getHostBpm()) + " BPM", theme::textDim);
    if (has)
        sub (projectCard, d.global.syncToHost ? "Chops follow the project tempo" : "Sync is off (Whole sample panel)",
             d.global.syncToHost ? theme::good : theme::textFaint);
}

void ChopLabEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // Header
    g.setColour (theme::panelRaised);
    g.fillRect (header);
    g.setColour (theme::outline);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    auto h = header.reduced (14, 0);
    g.setColour (theme::accent);
    g.setFont (theme::font (19.0f, true));
    g.drawText ("CHOP", h.removeFromLeft (52), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawText ("LAB", h.removeFromLeft (44), juce::Justification::centredLeft);

    const auto& d = proc.doc();
    auto info = h.withTrimmedLeft (loadButton.getRight() - h.getX() + 14);
    if (d.hasSample())
    {
        const double secs = d.sample->lengthSeconds();
        const juce::String length = juce::String ((int) secs / 60) + ":" + juce::String (std::fmod (secs, 60.0), 1).paddedLeft ('0', 4);
        g.setColour (theme::text);
        g.setFont (theme::font (14.5f, true));
        const auto name = d.sample->name + (d.reversed ? " (reversed)" : "");
        g.drawText (name, info, juce::Justification::centredLeft, true);
        const int nameW = juce::jmin (info.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (theme::font (14.5f, true), name) + 12);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText (length + "  |  " + juce::String (d.sample->sampleRate / 1000.0, 1) + " kHz  |  "
                        + (d.sample->audio.getNumChannels() > 1 ? "stereo" : "mono") + "  |  " + juce::String (d.slices.size()) + " chops",
                    info.withTrimmedLeft (nameW), juce::Justification::centredLeft, true);
    }

    if (status.isNotEmpty())
    {
        g.setColour (statusIsError ? juce::Colour (0xffff6b6b) : theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText (status, statusArea, juce::Justification::centredRight, true);
    }

    paintCards (g);
    if (proc.currentPage != 0)
        return;

    // Chop bar
    g.setColour (theme::panel);
    g.fillRoundedRectangle (chopBar.toFloat(), 8.0f);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("CHOP BY", chopCaption, juce::Justification::centredLeft);
    if (d.chop.mode == ChopMode::transients)
    {
        g.drawText ("SENSITIVITY", sensCaption, juce::Justification::centredRight);
        g.drawText ("MIN LENGTH", gapCaption, juce::Justification::centredRight);
    }
    else if (d.chop.mode == ChopMode::grid)
    {
        g.drawText ("EVERY", gridCaption, juce::Justification::centredRight);
    }
    else
    {
        g.setFont (theme::font (12.5f));
        g.drawText ("Double-click the waveform to add a chop, drag markers to move, right-click to remove", manualHint,
                    juce::Justification::centredLeft, true);
    }
}

void ChopLabEditor::paintOverChildren (juce::Graphics& g)
{
    if (! fileDragHover)
        return;
    g.setColour (theme::background.withAlpha (0.7f));
    g.fillAll();
    g.setColour (theme::accent);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (6.0f), 10.0f, 3.0f);
    g.setFont (theme::font (24.0f, true));
    g.drawText ("Drop to load and analyze", getLocalBounds(), juce::Justification::centred);
}

void ChopLabEditor::resized()
{
    auto r = getLocalBounds();
    header = r.removeFromTop (46);
    {
        auto h = header.reduced (14, 8);
        h.removeFromLeft (110);
        loadButton.setBounds (h.removeFromLeft (112));
        redoButton.setBounds (h.removeFromRight (58));
        h.removeFromRight (4);
        undoButton.setBounds (h.removeFromRight (58));
        h.removeFromRight (16);
        rollTab.setBounds (h.removeFromRight (96));
        chopsTab.setBounds (h.removeFromRight (80));
        h.removeFromRight (12);
        statusArea = h.removeFromRight (juce::jmin (420, h.getWidth() / 2));
    }

    r.reduce (10, 10);
    cards = r.removeFromTop (78);
    {
        auto c = cards;
        const int gap = 10;
        const int w = (c.getWidth() - 4 * gap) / 5;
        tempoCard = c.removeFromLeft (w);
        c.removeFromLeft (gap);
        keyCard = c.removeFromLeft (w);
        c.removeFromLeft (gap);
        timeCard = c.removeFromLeft (w);
        c.removeFromLeft (gap);
        lyricsCard.setBounds (c.removeFromLeft (w + 40));
        c.removeFromLeft (gap);
        projectCard = c;

        auto t = tempoCard.reduced (12, 0).withTrimmedTop (22).withHeight (32);
        doubleButton.setBounds (t.removeFromRight (36).reduced (0, 3));
        t.removeFromRight (4);
        halfButton.setBounds (t.removeFromRight (36).reduced (0, 3));
        bpmValue.setBounds (t.withWidth (juce::jmin (t.getWidth() - 40, 120)));

        timeSigBox.setBounds (timeCard.reduced (12, 0).withTrimmedTop (24).withHeight (28).withWidth (140));
    }
    r.removeFromTop (10);
    pageArea = r;
    patternView.setBounds (r);

    const int bottomH = juce::jlimit (280, 340, r.getHeight() * 2 / 5);
    auto bottom = r.removeFromBottom (bottomH);
    r.removeFromBottom (10);
    chopBar = r.removeFromBottom (44);
    r.removeFromBottom (10);
    waveform.setBounds (r);

    {
        auto b = chopBar.reduced (12, 8);
        chopCaption = b.removeFromLeft (62);
        for (auto* m : { &modeTransients, &modeGrid, &modeManual })
        {
            m->setBounds (b.removeFromLeft (86));
            b.removeFromLeft (4);
        }

        exportButton.setBounds (b.removeFromRight (112));
        b.removeFromRight (8);
        midiDrag.setBounds (b.removeFromRight (146));
        b.removeFromRight (8);
        stopButton.setBounds (b.removeFromRight (52));
        b.removeFromRight (4);
        playAllButton.setBounds (b.removeFromRight (70));
        b.removeFromRight (12);

        b.removeFromLeft (10);
        manualHint = b;
        auto modeArea = b;
        const int sliderW = juce::jmin (200, (modeArea.getWidth() - 2 * 86) / 2);
        sensCaption = modeArea.removeFromLeft (80);
        modeArea.removeFromLeft (6);
        sensitivity.setBounds (modeArea.removeFromLeft (sliderW));
        gapCaption = modeArea.removeFromLeft (80);
        modeArea.removeFromLeft (6);
        minGap.setBounds (modeArea.removeFromLeft (sliderW));

        auto gridArea = b;
        gridCaption = gridArea.removeFromLeft (44);
        gridArea.removeFromLeft (6);
        gridBox.setBounds (gridArea.removeFromLeft (140));
    }

    globalPanel.setBounds (bottom.removeFromRight (310));
    bottom.removeFromRight (10);
    inspector.setBounds (bottom.removeFromRight (380));
    bottom.removeFromRight (10);
    table.setBounds (bottom);
}
