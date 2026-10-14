#pragma once

#include "Theory.h"
#include <QString>
#include <QStringList>
#include <QVector>

namespace OpenDaw::Patterns {

struct Note {
    int pitch = 60;
    double startBeat = 0.0;     // relative to the pattern start
    double lengthBeats = 0.25;
    int velocity = 90;
};

struct Spec {
    QStringList chords;          // symbols or Roman numerals, one per segment, cycled
    QString key;                 // needed for Roman numerals; optional otherwise
    QString pattern = "up";      // up, down, up_down, down_up, random, chord
    QVector<int> steps;          // 1-based chord-tone indices; overrides `pattern`
    QString rate = "1/8";        // 1/4, 1/8, 1/16, 1/32, plus a "t" suffix for triplets
    int octave = 4;              // octave of the chord roots (C4 = 60)
    int octaves = 1;             // how many octaves the chord tones span
    double beatsPerChord = 4.0;
    double totalBeats = 0.0;     // 0 = one segment per chord
    int velocity = 90;
    int accent = 12;             // added to steps that fall on a beat
    double gate = 0.9;           // note length as a fraction of a step
    double swing = 0.0;          // 0-100; 100 delays every second step to a triplet feel
    int seed = 1;                // for the random pattern
};

struct Result {
    QVector<Note> notes;
    QStringList resolvedChords;  // e.g. "Gm: G4 Bb4 D5"
    QString error;               // empty on success
};

// Step length in beats for a rate such as "1/8" or "1/16t"; 0 if unknown.
double rateToBeats(const QString& rate);

Result generate(const Spec& spec);

} // namespace OpenDaw::Patterns
