#include "Pattern.h"

#include <algorithm>
#include <cmath>

namespace choplab
{

double barBeats (const TimeSignature& ts)
{
    return juce::jmax (0.25, ts.quarterBeatsPerBar());
}

double snapBeat (double beat, double snap)
{
    return snap > 0.0 ? std::round (beat / snap) * snap : beat;
}

Pattern patternFromSampleOrder (const Document& d)
{
    Pattern p;
    p.snapBeats = 0.25;
    if (! d.hasSample() || d.bpm <= 0.0)
        return p;

    const double bar = barBeats (d.timeSig);
    const double downbeat = d.downbeatSeconds * d.bpm / 60.0;
    // Shift so the sample's bar 1 lands on a bar line (a pickup goes in the bar before it).
    const double shift = downbeat > 1.0e-6 ? std::ceil (downbeat / bar - 1.0e-6) * bar - downbeat : 0.0;

    for (int i = 0; i < (int) d.slices.size(); ++i)
    {
        const auto& s = d.slices[(size_t) i];
        PatternNote n;
        n.chop = i;
        n.start = d.secondsAt (s.start) * d.bpm / 60.0 + shift;
        n.length = juce::jmax (1.0 / 64.0, d.lengthInBeats (s));
        n.velocity = 0.8f;
        p.notes.push_back (n);
    }

    const double total = d.secondsAt (d.length()) * d.bpm / 60.0 + shift;
    p.lengthBeats = juce::jmax (bar, std::ceil (total / bar - 1.0e-3) * bar);
    return p;
}

void remapPatternForSplit (Pattern& p, int splitChop)
{
    for (auto& n : p.notes)
        if (n.chop > splitChop)
            ++n.chop;
}

void remapPatternForMerge (Pattern& p, int removedChop)
{
    for (auto& n : p.notes)
        if (n.chop >= removedChop)
            n.chop = juce::jmax (0, n.chop - 1);
}

juce::MidiFile patternToMidi (const Pattern& p, int rootNote, double bpm, TimeSignature ts, int numChops)
{
    constexpr int ppq = 960;
    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::llround (60000000.0 / juce::jmax (1.0, bpm))), 0);
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (ts.numerator, ts.denominator), 0);

    for (const auto& n : p.notes)
    {
        const int note = rootNote + n.chop;
        if (n.chop < 0 || n.chop >= numChops || note > 127)
            continue;
        const double t0 = std::round (n.start * ppq);
        const double t1 = juce::jmax (t0 + 1.0, std::round (n.end() * ppq) - 1.0);
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, (int) std::lround (n.velocity * 127.0f));
        track.addEvent (juce::MidiMessage::noteOn (1, note, velocity), t0);
        track.addEvent (juce::MidiMessage::noteOff (1, note), t1);
    }
    track.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ppq);
    midi.addTrack (track);
    return midi;
}

} // namespace choplab
