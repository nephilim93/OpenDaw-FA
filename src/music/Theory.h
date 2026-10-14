#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace OpenDaw::Theory {

// Pitch class 0-11 (C = 0) from a note letter with optional accidentals:
// "C", "F#", "Bb", "Ebb", "c#". Unicode sharps and flats are accepted.
std::optional<int> parsePitchClass(const QString& name);

// MIDI note number from scientific pitch notation, middle C = C4 = 60:
// "G4" = 67, "Bb3" = 58, "C-1" = 0.
std::optional<int> parseNote(const QString& name);

// "G4", "Bb3" (flats are used for the black keys).
QString noteName(int midiNote);

struct Scale {
    int root = 0;               // pitch class
    QVector<int> intervals;     // semitones above the root, ascending, starting at 0
    QString name;               // e.g. "G natural minor"

    bool contains(int midiNote) const;
    // The nearest note in the scale; on a tie the lower one.
    int snap(int midiNote) const;
};

// A key or scale: "G minor", "Gm", "Bb major", "D dorian", "E harmonic minor",
// "A minor pentatonic", "C blues". A bare note means major.
std::optional<Scale> parseScale(const QString& key);

struct Chord {
    int root = 0;               // pitch class
    QVector<int> intervals;     // semitones above the root, ascending, starting at 0
    QString symbol;             // as given
};

// A chord symbol ("Gm", "Ebmaj7", "F7", "Dm7b5", "Csus4", "A/C#") or, when a
// key is given, a Roman numeral ("i", "VI", "V7", "ii°", "bVII"). For slash
// chords the bass note is ignored.
std::optional<Chord> parseChord(const QString& symbol, const std::optional<Scale>& key);

} // namespace OpenDaw::Theory
