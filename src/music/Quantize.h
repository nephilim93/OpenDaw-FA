#pragma once

#include "Theory.h"
#include <optional>

namespace OpenDaw::Quantize {

struct Options {
    double grid = 0.25;          // in beats (1/16 = 0.25)
    double strength = 1.0;       // 0-1: how far notes move toward the grid
    double swing = 0.0;          // 0-100: delays every second grid line, 100 = triplet feel
    bool lengths = false;        // also round note lengths to the grid
    std::optional<Theory::Scale> scale; // when set, pitches move to the nearest scale note
};

struct Note {
    int pitch = 60;
    double startBeat = 0.0;
    double lengthBeats = 0.25;
};

// The note after quantizing. Pitch snapping is all or nothing; strength
// applies to timing only.
Note apply(const Note& note, const Options& options);

// Grid size in beats for "1/16", "1/8t", "bar" and the like; 0 if unknown.
double gridToBeats(const QString& grid);

} // namespace OpenDaw::Quantize
