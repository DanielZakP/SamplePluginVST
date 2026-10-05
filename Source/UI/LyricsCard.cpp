#include "LyricsCard.h"
#include "Theme.h"
#include "Widgets.h"

namespace choplab
{

LyricsCard::LyricsCard (ChopLabProcessor& p) : proc (p)
{
    findButton.setTooltip ("Listen for words in the sample and put them on the chops they fall in. "
                           "Works best on clean vocals: separate the vocal with FL's stem separation first if it's over a beat.");
    findButton.onClick = [this]
    {
        const auto phase = proc.getLyricsStatus().phase;
        if (phase == ChopLabProcessor::LyricsStatus::downloading || phase == ChopLabProcessor::LyricsStatus::transcribing)
            proc.cancelLyrics();
        else
            proc.findLyrics();
    };
    addAndMakeVisible (findButton);

    std::vector<std::pair<juce::String, juce::String>> sorted;
    for (const auto& l : lyricsLanguages())
        sorted.push_back ({ l.name, l.code });
    std::sort (sorted.begin(), sorted.end());
    language.addItem ("Auto-detect", 1);
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        language.addItem (sorted[i].first, (int) i + 2);
        codes.push_back (sorted[i].second);
    }
    language.setTooltip ("Language of the vocals. Auto-detect works for most songs; pick one if it guesses wrong");
    language.onChange = [this]
    {
        const int id = language.getSelectedId();
        proc.setLyricsOptions (proc.doc().lyrics.model, id >= 2 ? codes[(size_t) id - 2] : juce::String());
    };
    addAndMakeVisible (language);

    for (int i = 0; i < (int) lyricsModels().size(); ++i)
        model.addItem (lyricsModels()[(size_t) i].name, i + 1);
    model.setTooltip ("Fast is fine for clear vocals. Accurate is better on sung or mumbled words, but slower. "
                      "Each is downloaded once, the first time you use it");
    model.onChange = [this] { proc.setLyricsOptions (model.getSelectedId() - 1, proc.doc().lyrics.requestedLanguage); };
    addAndMakeVisible (model);

    startTimerHz (8);
}

void LyricsCard::documentChanged()
{
    const auto& d = proc.doc();
    const auto status = proc.getLyricsStatus();
    const bool running = status.phase == ChopLabProcessor::LyricsStatus::downloading
                         || status.phase == ChopLabProcessor::LyricsStatus::transcribing;

    findButton.setButtonText (running ? "Cancel" : d.lyrics.searched ? "Find again" : "Find lyrics");
    findButton.setEnabled (d.hasSample() || running);

    language.changeItemText (1, d.lyrics.requestedLanguage.isEmpty() && d.lyrics.language.isNotEmpty()
                                    ? "Auto (" + lyricsLanguageName (d.lyrics.language) + ")"
                                    : juce::String ("Auto-detect"));
    int id = 1;
    for (size_t i = 0; i < codes.size(); ++i)
        if (codes[i] == d.lyrics.requestedLanguage)
            id = (int) i + 2;
    language.setSelectedId (id, juce::dontSendNotification);
    model.setSelectedId (d.lyrics.model + 1, juce::dontSendNotification);
    language.setEnabled (! running);
    model.setEnabled (! running);
    lastPhase = status.phase;
    repaint();
}

void LyricsCard::timerCallback()
{
    const auto status = proc.getLyricsStatus();
    if (status.phase != lastPhase)
        documentChanged();
    else if (status.phase == ChopLabProcessor::LyricsStatus::downloading || status.phase == ChopLabProcessor::LyricsStatus::transcribing)
        repaint (statusArea);
}

juce::String LyricsCard::statusText (juce::Colour& colour) const
{
    const auto& d = proc.doc();
    const auto s = proc.getLyricsStatus();
    colour = theme::textDim;
    const auto percent = juce::String (juce::roundToInt (s.progress * 100.0f)) + "%";
    switch (s.phase)
    {
        case ChopLabProcessor::LyricsStatus::downloading:
            return "Downloading the speech model... " + percent;
        case ChopLabProcessor::LyricsStatus::transcribing:
            return "Listening... " + percent;
        case ChopLabProcessor::LyricsStatus::failed:
            colour = juce::Colour (0xffff6b6b);
            return s.message;
        case ChopLabProcessor::LyricsStatus::done:
            if (d.lyrics.words.empty())
                return "No words found";
            colour = theme::good;
            return lyricsLanguageName (d.lyrics.language) + ", " + juce::String (d.lyrics.words.size()) + " words";
        case ChopLabProcessor::LyricsStatus::idle:
        default:
            if (! d.hasSample())
                return {};
            if (! lyricsSupportedOnThisCpu())
                return "Needs a CPU with AVX2";
            if (! lyricsModelFile (d.lyrics.model).existsAsFile())
                return "First run downloads the model once";
            return "Not searched yet";
    }
}

void LyricsCard::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    drawCaption (g, captionArea, "Lyrics");

    juce::Colour colour;
    const auto text = statusText (colour);
    const auto s = proc.getLyricsStatus();
    if (s.phase == ChopLabProcessor::LyricsStatus::downloading || s.phase == ChopLabProcessor::LyricsStatus::transcribing)
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

void LyricsCard::resized()
{
    auto r = getLocalBounds().reduced (12, 0);
    auto top = r.removeFromTop (28).withTrimmedTop (5);
    model.setBounds (top.removeFromRight (juce::jmin (130, top.getWidth() / 2)).withHeight (20));
    captionArea = top.withTrimmedTop (3);
    auto row = r.removeFromTop (26);
    findButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (6);
    language.setBounds (row);
    statusArea = r.withTrimmedTop (2).withTrimmedBottom (4);
}

} // namespace choplab
