#pragma once

#include "Patterns.h"
#include <QString>
#include <QVector>

// Idiomatic drum grooves by style, as General MIDI drum notes (play them on
// MIDI channel 10). Generation is deterministic: the same Spec always gives
// the same notes.
namespace OpenDaw::DrumPatterns {

// General MIDI percussion key map.
constexpr int kBassDrum     = 35;
constexpr int kKick         = 36;
constexpr int kSideStick    = 37;
constexpr int kSnare        = 38;
constexpr int kClap         = 39;
constexpr int kElectricSnare = 40;
constexpr int kLowFloorTom  = 41;
constexpr int kClosedHat    = 42;
constexpr int kHighFloorTom = 43;
constexpr int kPedalHat     = 44;
constexpr int kLowTom       = 45;
constexpr int kOpenHat      = 46;
constexpr int kLowMidTom    = 47;
constexpr int kHighMidTom   = 48;
constexpr int kCrash        = 49;
constexpr int kHighTom      = 50;
constexpr int kRide         = 51;
constexpr int kChina        = 52;
constexpr int kRideBell     = 53;
constexpr int kTambourine   = 54;
constexpr int kSplash       = 55;
constexpr int kCowbell      = 56;
constexpr int kCrash2       = 57;
constexpr int kRide2        = 59;
constexpr int kHighBongo    = 60;
constexpr int kLowBongo     = 61;
constexpr int kMuteConga    = 62;
constexpr int kOpenConga    = 63;
constexpr int kLowConga     = 64;
constexpr int kHighTimbale  = 65;
constexpr int kLowTimbale   = 66;
constexpr int kCabasa       = 69;
constexpr int kMaracas      = 70;
constexpr int kClaves       = 75;
constexpr int kHighWoodBlock = 76;
constexpr int kLowWoodBlock = 77;

struct StyleInfo {
    QString name;
    QString description;
    int beatsPerBar = 4;
    double defaultSwing = 0.0;  // 0-100
    int tempoMin = 90;          // typical tempo range, BPM
    int tempoMax = 130;
};

QVector<StyleInfo> styles();

struct Spec {
    QString style;
    int bars = 4;
    double intensity = 0.5;   // 0-1: sparse to busy
    double swing = -1;        // 0-100 (100 = full triplet); -1 = the style's default
    int fillEvery = 4;        // a fill ends every Nth bar and the last bar; 0 = no fills
    double fillBeats = 1;     // fill length at the end of a fill bar
    bool crashOnFirst = true;
    double humanize = 0;      // 0-1: timing (up to 10 ms at `tempo`) and velocity jitter
    int variation = 0;        // picks one of the style's variants (cycles)
    int seed = 1;             // picks fills and drives humanize
    double tempo = 120;       // BPM, only used to size the humanize jitter
};

struct Result {
    QVector<Patterns::Note> notes;  // sorted by start
    QString summary;
    QString error;                  // empty on success
};

Result generate(const Spec& spec);

} // namespace OpenDaw::DrumPatterns
