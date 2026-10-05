#pragma once

#include "Model.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace choplab
{

// The chops in their original order and timing, with bar 1 of the sample on a bar line.
Pattern patternFromSampleOrder (const Document&);

// Keep notes pointing at the same audio when a chop is split in two or merged into the previous one.
void remapPatternForSplit (Pattern&, int splitChop);
void remapPatternForMerge (Pattern&, int removedChop);

double snapBeat (double beat, double snap);
double barBeats (const TimeSignature&);

// MIDI file of the pattern: chop n plays on rootNote + n.
juce::MidiFile patternToMidi (const Pattern&, int rootNote, double bpm, TimeSignature, int numChops);

// Notes starting or ending in [fromBeat, fromBeat + spanBeats) of a looping pattern. The
// callback gets the note index, the beat offset from fromBeat, and whether it's a note-on.
template <typename Callback>
void forEachPatternEvent (const std::vector<PatternNote>& notes, double lengthBeats, double fromBeat, double spanBeats, Callback&& cb)
{
    if (lengthBeats <= 0.0 || spanBeats <= 0.0)
        return;
    const double from = std::fmod (fromBeat, lengthBeats);
    auto visit = [&] (double t, size_t index, bool on)
    {
        // t is a position within the loop; find its offset from `from`, allowing for wrap-around
        double offset = t - from;
        if (offset < 0.0)
            offset += lengthBeats;
        if (offset < spanBeats)
            cb (index, offset, on);
    };
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        if (n.start >= lengthBeats)
            continue;
        visit (n.start, i, true);
        visit (std::fmod (juce::jmin (n.end(), lengthBeats), lengthBeats), i, false);
    }
}

} // namespace choplab
