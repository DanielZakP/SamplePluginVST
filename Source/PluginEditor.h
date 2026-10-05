#pragma once

#include "PluginProcessor.h"
#include "UI/LyricsCard.h"
#include "UI/Panels.h"
#include "UI/PatternView.h"
#include "UI/SliceTable.h"
#include "UI/Theme.h"
#include "UI/WaveformView.h"
#include "UI/Widgets.h"

class ChopLabEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit ChopLabEditor (ChopLabProcessor&);
    ~ChopLabEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void refresh();
    void chooseFile();
    void exportAll();
    void setStatus (const juce::String&, bool isError = false);
    void chopClicked (int index);
    void selectAndPlay (int index);
    juce::File exportChop (int index);
    void setMode (choplab::ChopMode);
    void paintCards (juce::Graphics&);
    void showPage (int page);

    ChopLabProcessor& proc;
    choplab::theme::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 700 };

    juce::TextButton loadButton { "Load sample" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton chopsTab { "Chops" }, rollTab { "Piano roll" };

    juce::Label bpmValue;
    juce::TextButton halfButton { "/2" }, doubleButton { "x2" };
    juce::ComboBox timeSigBox;

    choplab::WaveformView waveform;

    juce::TextButton modeTransients { "Transients" }, modeGrid { "Grid" }, modeManual { "Manual" };
    juce::Slider sensitivity, minGap;
    juce::ComboBox gridBox;
    juce::TextButton playAllButton { "Play all" }, stopButton { "Stop" }, exportButton { "Export chops..." };
    choplab::DragOutButton midiDrag;

    choplab::SliceTable table;
    choplab::SliceInspector inspector;
    choplab::GlobalPanel globalPanel;
    choplab::LyricsCard lyricsCard;
    choplab::PatternView patternView;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::String status;
    bool statusIsError = false;
    juce::uint32 statusTime = 0;
    bool fileDragHover = false;
    double shownHostBpm = 0.0;

    juce::Rectangle<int> header, cards, tempoCard, keyCard, timeCard, projectCard, pageArea, chopBar, statusArea, chopCaption, sensCaption, gapCaption,
        gridCaption, manualHint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChopLabEditor)
};
