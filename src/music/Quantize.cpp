#include "Quantize.h"
#include "Patterns.h"
#include <algorithm>
#include <cmath>

namespace OpenDaw::Quantize {

static constexpr double kMinLength = 0.03125;

Note apply(const Note& note, const Options& options)
{
    Note out = note;
    const double grid = options.grid;
    const double strength = std::clamp(options.strength, 0.0, 1.0);

    if (grid > 0.0) {
        const long long line = std::llround(note.startBeat / grid);
        double target = line * grid;
        if (line % 2 != 0)
            target += std::clamp(options.swing, 0.0, 100.0) / 100.0 * grid / 3.0;
        out.startBeat = std::max(0.0, note.startBeat + (target - note.startBeat) * strength);

        if (options.lengths) {
            const double snapped = std::max(grid, std::round(note.lengthBeats / grid) * grid);
            out.lengthBeats = note.lengthBeats + (snapped - note.lengthBeats) * strength;
        }
    }
    out.lengthBeats = std::max(kMinLength, out.lengthBeats);

    if (options.scale)
        out.pitch = std::clamp(options.scale->snap(note.pitch), 0, 127);
    return out;
}

double gridToBeats(const QString& grid)
{
    const QString g = grid.trimmed().toLower();
    if (g == "bar" || g == "1/1") return 4.0;
    return Patterns::rateToBeats(g);
}

} // namespace OpenDaw::Quantize
