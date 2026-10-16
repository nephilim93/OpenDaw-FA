#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

// The Drum Machine's kit: which sound each MIDI note plays. No Tracktion or
// JUCE here (like src/modular), so it is testable on its own.
namespace OpenDaw::Drums {

// General MIDI percussion notes (channel 10).
namespace Gm {
inline constexpr int Kick = 36, SideStick = 37, Snare = 38, Clap = 39, ElectricSnare = 40;
inline constexpr int LowFloorTom = 41, ClosedHat = 42, HighFloorTom = 43, PedalHat = 44;
inline constexpr int LowTom = 45, OpenHat = 46, LowMidTom = 47, HighMidTom = 48;
inline constexpr int Crash = 49, HighTom = 50, Ride = 51, Chinese = 52, RideBell = 53;
inline constexpr int Tambourine = 54, Splash = 55, Cowbell = 56, Crash2 = 57;
} // namespace Gm

enum class PadSource { Synth, Sample };

struct Pad {
    int note = Gm::Kick;
    QString name;
    PadSource source = PadSource::Synth;
    // Synth pads: a modular module with a "trigger" input and an "out" output
    // (kick, snare, hihat, ...), and its parameter values (others default).
    QString module;
    QMap<QString, float> params;
    // Sample pads: an audio file (wav, aiff, flac, ogg).
    QString sample;

    float level = 0.8f;     // 0..1.5, linear gain
    float pan = 0.0f;       // -1 (left) .. 1 (right)
    float tune = 0.0f;      // semitones, -24..24
    int choke = 0;          // pads in the same non-zero group cut each other off
};

struct Kit {
    QVector<Pad> pads;

    static std::optional<Kit> fromJson(const QJsonObject& json, QString* error);
    QJsonObject toJson() const;

    // Errors make the kit unusable (each with a fix hint).
    QStringList validate() const;

    const Pad* padForNote(int note) const;
    Pad* padForNote(int note);

    // A General MIDI kit made of synth voices: plays as soon as it's loaded.
    static Kit defaultKit();
};

const char* padSourceName(PadSource s);
// Modules a synth pad may use: the drum voices (the Voices family's modules
// with a "trigger" input and an "out" output).
QStringList synthPadModules();

} // namespace OpenDaw::Drums
