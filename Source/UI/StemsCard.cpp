#include "StemsCard.h"
#include "Theme.h"

namespace choplab
{
namespace
{
using Status = ChopLabProcessor::StemsStatus;

bool isRunning (int phase)
{
    return phase == Status::downloading || phase == Status::separating;
}

juce::String roughly (double seconds)
{
    if (seconds < 50.0)
        return juce::String (juce::jmax (5, juce::roundToInt (seconds / 5.0) * 5)) + " s";
    return juce::String (juce::jmax (1, juce::roundToInt (seconds / 60.0))) + " min";
}
} // namespace

StemsCard::StemsCard (ChopLabProcessor& p)
    : proc (p), drag ("Drag all", [this] { return proc.stemFile (proc.doc().source); })
{
    separateButton.setTooltip ("Split the sample into vocals, drums, bass and everything else, so the chops can play just one of them. "
                               "It runs on your CPU and takes a while: roughly a minute for a 30-second sample on a fast machine");
    separateButton.onClick = [this]
    {
        if (isRunning (proc.getStemsStatus().phase))
            proc.cancelStems();
        else
            proc.separateStems();
    };
    addAndMakeVisible (separateButton);

    source.addItem ("Full mix", 1);
    for (int i = 0; i < kNumStems; ++i)
        source.addItem (stemName (i), i + 2);
    source.setTooltip ("What the chops play. Chop points, tempo and lyrics stay the same whichever you pick");
    source.onChange = [this] { proc.setSource (source.getSelectedId() - 2); };
    addChildComponent (source);

    drag.makeFiles = [this] { return filesToDrag(); };
    drag.setTooltip ("Drag into FL's playlist or channel rack. With Full mix picked it drags vocals, drums, bass and other together. "
                     "Click to show the files");
    drag.onClick = [this]
    {
        const auto files = filesToDrag();
        if (! files.isEmpty())
            juce::File (files[0]).revealToUser();
    };
    addChildComponent (drag);

    startTimerHz (4);
}

juce::StringArray StemsCard::filesToDrag() const
{
    juce::StringArray files;
    const int playing = proc.doc().source;
    for (int stem : { stemVocals, stemDrums, stemBass, stemOther, stemInstrumental })
        if (playing < 0 ? stem != stemInstrumental : stem == playing)
            if (const auto f = proc.stemFile (stem); f.existsAsFile())
                files.add (f.getFullPathName());
    return files;
}

void StemsCard::documentChanged()
{
    const auto& d = proc.doc();
    const auto status = proc.getStemsStatus();
    const bool running = isRunning (status.phase);
    const bool separated = d.stems != nullptr;

    separateButton.setVisible (! separated);
    separateButton.setButtonText (running ? "Cancel" : "Separate stems");
    separateButton.setEnabled ((d.hasSample() && ! proc.isAnalysing()) || running);
    source.setVisible (separated);
    drag.setVisible (separated);
    source.setSelectedId (d.source + 2, juce::dontSendNotification);
    source.setEnabled (! proc.isAnalysing());
    drag.setText (d.source >= 0 ? "Drag " + stemName (d.source).toLowerCase() : juce::String ("Drag all"));

    if (status.phase == Status::separating && lastPhase != Status::separating)
        separatingSince = juce::Time::getMillisecondCounterHiRes();
    lastPhase = status.phase;
    repaint();
}

void StemsCard::timerCallback()
{
    const auto status = proc.getStemsStatus();
    if (status.phase != lastPhase)
        documentChanged();
    else if (isRunning (status.phase))
        repaint (statusArea);
}

juce::String StemsCard::statusText (juce::Colour& colour) const
{
    const auto& d = proc.doc();
    const auto s = proc.getStemsStatus();
    colour = theme::textDim;
    const auto percent = juce::String (juce::roundToInt (s.progress * 100.0f)) + "%";
    switch (s.phase)
    {
        case Status::downloading:
            return "Downloading model... " + percent;
        case Status::separating:
        {
            auto text = "Separating... " + percent;
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - separatingSince) / 1000.0;
            if (s.progress > 0.04f && elapsed > 5.0)
                text << ", about " << roughly (elapsed * (1.0 - s.progress) / s.progress) << " left";
            return text;
        }
        case Status::failed:
            colour = theme::error;
            return s.message;
        case Status::done:
        {
            int own = 0;
            for (const auto& slice : d.slices)
                own += slice.settings.stem >= 0 ? 1 : 0;
            return own > 0 ? juce::String (own) + (own == 1 ? " chop plays" : " chops play") + " its own stem" : juce::String();
        }
        case Status::idle:
        default:
            if (! d.hasSample())
                return {};
            if (! stemsSupportedOnThisCpu())
                return "Needs a CPU with AVX2";
            if (! stemsModelLooksComplete())
                return "Downloads 84 MB on first use";
            return {};
    }
}

void StemsCard::paint (juce::Graphics& g)
{
    drawCaption (g, captionArea, "Stems");

    juce::Colour colour;
    const auto text = statusText (colour);
    setTooltip (proc.getStemsStatus().phase == Status::failed ? text : juce::String()); // errors can be longer than the line
    const auto s = proc.getStemsStatus();
    if (isRunning (s.phase))
    {
        auto bar = statusArea.toFloat().withHeight (3.0f).withY ((float) statusArea.getBottom() - 3.0f);
        g.setColour (theme::outline);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (theme::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, s.progress)), 1.5f);
    }
    g.setColour (colour);
    g.setFont (theme::font (12.5f));
    g.drawText (text, statusArea.withTrimmedBottom (4), juce::Justification::centredLeft, true);
}

void StemsCard::resized()
{
    auto r = getLocalBounds().reduced (12, 0);
    captionArea = r.removeFromTop (28).withTrimmedTop (8);
    auto row = r.removeFromTop (26);
    separateButton.setBounds (row.withWidth (juce::jmin (130, row.getWidth())));
    drag.setBounds (row.removeFromRight (juce::jmin (112, row.getWidth() / 2)));
    row.removeFromRight (6);
    source.setBounds (row);
    statusArea = r.withTrimmedTop (2).withTrimmedBottom (4);
}

} // namespace choplab
