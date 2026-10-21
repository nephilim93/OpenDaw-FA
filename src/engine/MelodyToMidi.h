#pragma once

#include "engine/EditManager.h"
#include "music/MelodyTranscriber.h"
#include <QStringList>
#include <optional>

namespace OpenDaw {

// Turns a hummed, sung or whistled audio clip into a MIDI clip: reads the
// part of the file the clip plays, transcribes it (src/music), and places
// the notes on the project's beat grid.
namespace MelodyToMidi {

inline constexpr double kMaxSeconds = 180.0;

struct Transcription {
    Melody::Result result;
    double clipStartSeconds = 0.0;   // where the audio clip starts on the timeline
    double clipLengthSeconds = 0.0;
    QStringList warnings;
};

std::optional<Transcription> transcribe(te::WaveAudioClip& clip, const Melody::Options& options, QString* error);

struct Converted {
    Transcription transcription;
    te::AudioTrack* track = nullptr;   // the new MIDI track
    te::MidiClip* clip = nullptr;
    int clipIndex = 0;
};

// The notes in a new MIDI track ("<track> Melody", with the Modular Synth so
// it plays at once), at the audio clip's place, as one undo step named
// undoName. Nothing changes when no notes were found (the error says why).
std::optional<Converted> convert(EditManager& editMgr, te::WaveAudioClip& clip, const Melody::Options& options,
                                 const juce::String& undoName, QString* error);

// A note's place on the beat grid.
double toBeats(te::Edit& edit, double seconds);

} // namespace MelodyToMidi

} // namespace OpenDaw
