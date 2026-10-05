#include "SliceTable.h"
#include "Theme.h"

namespace choplab
{

class SliceTable::LabelCell : public juce::Label
{
public:
    explicit LabelCell (SliceTable& o) : owner (o)
    {
        setEditable (false, true, false);
        setFont (theme::font (13.5f));
        setColour (juce::Label::textColourId, theme::text);
        setMinimumHorizontalScale (1.0f);
        onTextChange = [this]
        {
            auto& p = owner.proc;
            if (row < 0 || row >= (int) p.doc().slices.size())
                return;
            auto s = p.doc().slices[(size_t) row].settings;
            s.label = getText().trim();
            p.setSliceSettings (row, s);
        };
    }

    void setRow (int r)
    {
        row = r;
        const auto& slices = owner.proc.doc().slices;
        const auto label = r >= 0 && r < (int) slices.size() ? slices[(size_t) r].settings.label : juce::String();
        setText (label, juce::dontSendNotification);
        setTooltip ("Double-click to name this chop");
    }

    void paint (juce::Graphics& g) override
    {
        if (! isBeingEdited() && getText().isEmpty())
        {
            g.setColour (theme::textFaint);
            g.setFont (getFont());
            g.drawText ("add label", getLocalBounds().reduced (4, 0), juce::Justification::centredLeft, true);
            return;
        }
        juce::Label::paint (g);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.table.selectRow (row);
        owner.cellClicked (row, colLabel, e);
        juce::Label::mouseDown (e);
    }

private:
    SliceTable& owner;
    int row = -1;
};

//==============================================================================
SliceTable::SliceTable (ChopLabProcessor& p) : proc (p)
{
    addAndMakeVisible (table);
    table.setModel (this);
    table.setRowHeight (26);
    table.setHeaderHeight (24);
    table.setMultipleSelectionEnabled (false);
    table.setColour (juce::ListBox::backgroundColourId, theme::panel);

    auto& h = table.getHeader();
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
    h.addColumn ("#", colIndex, 36, 28, 60, columnFlags);
    h.addColumn ("Note", colNote, 50, 40, 80, columnFlags);
    h.addColumn ("Label", colLabel, 124, 70, 400, columnFlags);
    h.addColumn ("Pos", colBar, 60, 46, 120, columnFlags);
    h.addColumn ("On", colOn, 76, 56, 140, columnFlags);
    h.addColumn ("Beats", colLength, 54, 44, 120, columnFlags);
    h.addColumn ("Type", colType, 84, 56, 140, columnFlags);
    h.addColumn ("Chord", colHarmony, 56, 44, 100, columnFlags);
    h.addColumn ("Edits", colEdits, 96, 50, 400, columnFlags);
    h.setStretchToFitActive (true);
    h.setPopupMenuActive (false);
}

void SliceTable::documentChanged()
{
    table.updateContent();
    const int sel = proc.selectedSlice;
    if (sel != table.getSelectedRow())
    {
        updatingSelection = true;
        if (sel >= 0)
        {
            table.selectRow (sel);
            table.scrollToEnsureRowIsOnscreen (sel);
        }
        else
        {
            table.deselectAllRows();
        }
        updatingSelection = false;
    }
    table.repaint();
}

void SliceTable::resized()
{
    table.setBounds (getLocalBounds());
}

void SliceTable::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
}

int SliceTable::getNumRows()
{
    return (int) proc.doc().slices.size();
}

void SliceTable::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (theme::sliceColour (row).withAlpha (0.16f));
    else if (row % 2 == 1)
        g.fillAll (theme::panelRaised.withAlpha (0.45f));
    g.setColour (theme::sliceColour (row));
    g.fillRect (0, 3, 3, height - 6);
    g.setColour (theme::outline.withAlpha (0.5f));
    g.drawHorizontalLine (height - 1, 0.0f, (float) width);
}

juce::String SliceTable::editsSummary (const SliceSettings& s)
{
    juce::StringArray parts;
    if (std::abs (s.pitch) > 0.005f)
        parts.add ((s.pitch > 0 ? "+" : "") + juce::String (s.pitch, std::abs (s.pitch - std::round (s.pitch)) < 0.005f ? 0 : 2) + " st");
    if (std::abs (s.speed - 1.0f) > 0.001f)
        parts.add (juce::String (s.speed, 2) + "x" + (s.keepPitch ? "" : " tape"));
    if (s.reverse)
        parts.add ("reversed");
    if (std::abs (s.gainDb) > 0.05f)
        parts.add ((s.gainDb > 0 ? "+" : "") + juce::String (s.gainDb, 1) + " dB");
    if (s.attackMs > 0.5f)
        parts.add ("fade in");
    return parts.joinIntoString (" · ");
}

juce::String SliceTable::cellText (int row, int column) const
{
    const auto& d = proc.doc();
    if (row < 0 || row >= (int) d.slices.size())
        return {};
    const auto& s = d.slices[(size_t) row];

    switch (column)
    {
        case colIndex: return juce::String (row + 1);
        case colNote: return d.noteForSlice (row) >= 0 ? midiNoteName (d.noteForSlice (row)) : juce::String ("-");
        case colBar:
        {
            const auto p = d.gridPosition (s.start);
            if (p.bar < 1)
                return "pickup";
            return juce::String (p.bar) + "." + juce::String (p.beat) + "." + juce::String (p.sixteenth);
        }
        case colOn: return d.gridPosition (s.start).describe;
        case colLength:
        {
            const double beats = d.lengthInBeats (s);
            return juce::String (beats, beats < 10.0 ? 2 : 1);
        }
        case colType: return s.info.type;
        case colHarmony: return s.info.harmony.isNotEmpty() ? s.info.harmony : juce::String ("-");
        case colEdits: return editsSummary (s.settings);
        default: return {};
    }
}

void SliceTable::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    const auto text = cellText (row, column);
    const auto& d = proc.doc();
    juce::Colour c = theme::text;
    juce::Font f = theme::font (13.5f);

    if (column == colIndex)
    {
        f = theme::font (13.0f, true);
        c = theme::sliceColour (row);
    }
    else if (column == colNote || column == colBar)
    {
        f = theme::mono (13.0f);
        c = theme::textDim;
    }
    else if (column == colOn)
    {
        const auto p = row < (int) d.slices.size() ? d.gridPosition (d.slices[(size_t) row].start) : GridPosition {};
        c = ! p.onGrid ? theme::textFaint : p.describe == "Bar start" ? theme::accent : p.sixteenth == 1 ? theme::text : theme::textDim;
        if (p.describe == "Bar start")
            f = theme::font (13.5f, true);
    }
    else if (column == colEdits || column == colLength || column == colHarmony || column == colType)
    {
        c = column == colEdits ? theme::warn : theme::textDim;
    }

    g.setColour (c);
    g.setFont (f);
    g.drawText (text, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
}

juce::Component* SliceTable::refreshComponentForCell (int row, int column, bool, juce::Component* existing)
{
    if (column != colLabel)
    {
        jassert (existing == nullptr);
        return nullptr;
    }
    auto* cell = dynamic_cast<LabelCell*> (existing);
    if (cell == nullptr)
    {
        delete existing;
        cell = new LabelCell (*this);
    }
    if (! cell->isBeingEdited())
        cell->setRow (row);
    return cell;
}

void SliceTable::cellClicked (int row, int, const juce::MouseEvent&)
{
    if (onChopClicked)
        onChopClicked (row);
}

void SliceTable::selectedRowsChanged (int lastRowSelected)
{
    if (! updatingSelection && lastRowSelected >= 0 && lastRowSelected != proc.selectedSlice)
        proc.selectSlice (lastRowSelected);
}

void SliceTable::deleteKeyPressed (int row)
{
    if (row > 0)
        proc.removeMarker (row); // merges the chop into the one before it
}

void SliceTable::returnKeyPressed (int row)
{
    if (onChopClicked)
        onChopClicked (row);
}

juce::String SliceTable::getCellTooltip (int row, int column)
{
    const auto& d = proc.doc();
    if (row < 0 || row >= (int) d.slices.size())
        return {};
    const auto& s = d.slices[(size_t) row];
    switch (column)
    {
        case colNote: return "Play this chop with " + cellText (row, colNote) + " in the piano roll";
        case colBar:
        case colOn:
        {
            const auto p = d.gridPosition (s.start);
            return "Starts " + juce::String (d.secondsAt (s.start), 3) + " s into the sample"
                   + (p.onGrid ? juce::String() : ", " + juce::String (std::abs (p.offsetMs), 0) + " ms "
                                                      + (p.offsetMs > 0 ? "after" : "before") + " the nearest 16th");
        }
        case colLength: return "Length: " + juce::String (d.lengthInBeats (s), 3) + " beats, " + juce::String (d.secondsAt (s.end - s.start), 3) + " s";
        case colType: return "Best guess from the sound's spectrum. Peak " + juce::String (s.info.peakDb, 1) + " dBFS";
        case colHarmony: return s.info.harmony.isNotEmpty() ? "Detected chord or note" : "No clear pitch in this chop";
        default: return {};
    }
}

} // namespace choplab
