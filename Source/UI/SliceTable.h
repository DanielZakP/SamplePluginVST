#pragma once

#include "../PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace choplab
{

// One row per chop: note, label (double-click to edit), position in the bar, length, what it sounds like.
class SliceTable : public juce::Component,
                   private juce::TableListBoxModel
{
public:
    explicit SliceTable (ChopLabProcessor&);

    void documentChanged();
    void resized() override;
    void paint (juce::Graphics&) override;

    std::function<void (int)> onChopClicked;

private:
    enum Columns
    {
        colIndex = 1,
        colNote,
        colLabel,
        colLyrics,
        colBar,
        colOn,
        colLength,
        colType,
        colHarmony,
        colEdits
    };

    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int column, bool selected, juce::Component* existing) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    juce::String getCellTooltip (int row, int column) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void returnKeyPressed (int lastRowSelected) override;

    juce::String cellText (int row, int column) const;
    static juce::String editsSummary (const SliceSettings&);

    class LabelCell;

    ChopLabProcessor& proc;
    juce::TableListBox table;
    bool updatingSelection = false;
};

} // namespace choplab
