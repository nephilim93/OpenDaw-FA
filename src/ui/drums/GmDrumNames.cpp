#include "GmDrumNames.h"
#include <array>

namespace OpenDaw::Drums {

QString gmDrumName(int note)
{
    static constexpr std::array<const char*, 47> kNames = {
        "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap",
        "Electric Snare", "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat",
        "Low Tom", "Open Hi-Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom",
        "Ride Cymbal 1", "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash Cymbal",
        "Cowbell", "Crash Cymbal 2", "Vibraslap", "Ride Cymbal 2", "Hi Bongo", "Low Bongo",
        "Mute Hi Conga", "Open Hi Conga", "Low Conga", "High Timbale", "Low Timbale",
        "High Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle",
        "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block", "Low Wood Block",
        "Mute Cuica", "Open Cuica", "Mute Triangle", "Open Triangle",
    };
    if (note >= 35 && note < 35 + int(kNames.size()))
        return kNames[size_t(note - 35)];
    return QString("Note %1").arg(note);
}

Pad defaultPadForNote(int note)
{
    if (const auto* pad = Kit::defaultKit().padForNote(note)) return *pad;
    Pad pad;
    pad.note = note;
    pad.name = gmDrumName(note);
    const auto voices = synthPadModules();
    pad.module = voices.contains("kick") ? QString("kick") : voices.value(0);
    return pad;
}

} // namespace OpenDaw::Drums
