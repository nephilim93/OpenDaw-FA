#pragma once

#include "DrumKit.h"
#include <QMap>
#include <QString>
#include <QStringList>
#include <optional>

// Builds a Drum Machine kit from a folder of samples by reading file and
// folder names ("Kicks/BD_Punchy.wav", "808 OH.wav", "Tom Floor 2.aif"):
// each file gets a General MIDI drum note when its name says what it is.
namespace OpenDaw::Drums {

// The GM note a sample's path suggests, or nothing. Words in the file name
// count more than words in the folders above it.
std::optional<int> guessDrumNote(const QString& relativePath);

// Audio files (wav, aiff, aif, flac, ogg) under `folder`, relative to it,
// sorted; at most `limit`.
QStringList listSampleFiles(const QString& folder, int limit = 2000, bool* truncated = nullptr);

struct FolderKit {
    Kit kit;
    // The sample each note got (kit paths: relative to the samples folder
    // when inside it), and the other files that matched the same note.
    QMap<int, QString> chosen;
    QMap<int, QStringList> alternatives;
    QStringList unmatched;   // files whose names say nothing we know
    int sampleCount = 0;     // files looked at
};

// Every recognised sound in `folder` (and its subfolders) on its GM note,
// keeping the default kit's level, pan and choke for that note. With
// `fillWithSynth`, notes no file matched keep the default kit's synth pads,
// so every groove still plays in full.
FolderKit buildKitFromFolder(const QString& folder, const QString& samplesFolder, bool fillWithSynth = true);

} // namespace OpenDaw::Drums
