#pragma once

#include <QString>

namespace OpenDaw::Drums {

// The General MIDI percussion name of a note (35..81), else "Note N".
QString gmDrumName(int note);
// The first GM percussion note with no pad (35..81), then any free note;
// -1 when all 128 are taken. `taken(note)` says whether a note has a pad.
template <typename Taken>
int nextFreeDrumNote(Taken taken)
{
    for (int n = 35; n <= 81; ++n)
        if (!taken(n)) return n;
    for (int n = 0; n < 128; ++n)
        if (!taken(n)) return n;
    return -1;
}

} // namespace OpenDaw::Drums

#include "drums/DrumKit.h"

namespace OpenDaw::Drums {

// What a pad on `note` starts as (added, reset, or switched back to a synth
// voice): the default kit's pad when it has one, else a kick voice named
// after the GM note.
Pad defaultPadForNote(int note);

} // namespace OpenDaw::Drums
