#include "Patterns.h"
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace OpenDaw::Patterns {

double rateToBeats(const QString& rate)
{
    QString r = rate.trimmed().toLower();
    const bool triplet = r.endsWith('t');
    if (triplet) r.chop(1);
    if (!r.startsWith("1/")) return 0.0;

    bool ok = false;
    const int division = r.mid(2).toInt(&ok);
    if (!ok || division <= 0 || division > 64) return 0.0;

    const double beats = 4.0 / division;    // a beat is a quarter note
    return triplet ? beats * 2.0 / 3.0 : beats;
}

namespace {

// The order in which chord tones are played for one segment.
QVector<int> toneOrder(const Spec& spec, int toneCount, int stepCount, QRandomGenerator& rng)
{
    QVector<int> order;
    if (!spec.steps.isEmpty()) {
        for (int step : spec.steps)
            order.append(step - 1);
        return order;
    }

    QVector<int> up;
    for (int i = 0; i < toneCount; ++i) up.append(i);
    QVector<int> down(up.rbegin(), up.rend());

    if (spec.pattern == "down") return down;
    if (spec.pattern == "up_down") {
        order = up;
        for (int i = toneCount - 2; i > 0; --i) order.append(i);
        return order;
    }
    if (spec.pattern == "down_up") {
        order = down;
        for (int i = 1; i < toneCount - 1; ++i) order.append(i);
        return order;
    }
    if (spec.pattern == "random") {
        for (int i = 0; i < stepCount; ++i)
            order.append(int(rng.bounded(toneCount)));
        return order;
    }
    return up;
}

} // namespace

Result generate(const Spec& spec)
{
    Result result;

    const double step = rateToBeats(spec.rate);
    if (step <= 0.0) {
        result.error = QString("Unknown rate '%1'. Use 1/4, 1/8, 1/16 or 1/32, with a 't' "
                               "suffix for triplets.").arg(spec.rate);
        return result;
    }
    if (spec.chords.isEmpty()) {
        result.error = "No chords given.";
        return result;
    }
    static const QStringList patterns = { "up", "down", "up_down", "down_up", "random", "chord" };
    if (spec.steps.isEmpty() && !patterns.contains(spec.pattern)) {
        result.error = QString("Unknown pattern '%1'. Use %2, or give steps.")
                           .arg(spec.pattern, patterns.join(", "));
        return result;
    }

    std::optional<Theory::Scale> key;
    if (!spec.key.trimmed().isEmpty()) {
        key = Theory::parseScale(spec.key);
        if (!key) {
            result.error = QString("Unknown key '%1'. Use e.g. 'G minor', 'Bb major', "
                                   "'D dorian'.").arg(spec.key);
            return result;
        }
    }

    // Chord tones as MIDI notes, spread over the requested octaves.
    QVector<QVector<int>> chordTones;
    QStringList unknown;
    for (const auto& symbol : spec.chords) {
        auto chord = Theory::parseChord(symbol, key);
        if (!chord) {
            unknown << symbol;
            continue;
        }
        const int root = (spec.octave + 1) * 12 + chord->root;
        QVector<int> tones;
        for (int o = 0; o < std::max(1, spec.octaves); ++o)
            for (int interval : chord->intervals)
                tones.append(root + interval + 12 * o);
        chordTones.append(tones);

        QStringList names;
        for (int t : chord->intervals) names << Theory::noteName(root + t);
        result.resolvedChords << QString("%1: %2").arg(symbol, names.join(' '));
    }
    if (!unknown.isEmpty()) {
        result.error = QString("Could not read chord(s): %1. Use symbols like Gm, Ebmaj7, "
                               "F7, Dm7b5, or Roman numerals (i, VI, V7) with a key.")
                           .arg(unknown.join(", "));
        return result;
    }

    const double total = spec.totalBeats > 0.0 ? spec.totalBeats
                                               : spec.beatsPerChord * spec.chords.size();
    const int segments = int(std::ceil(total / spec.beatsPerChord - 1e-9));
    QRandomGenerator rng(quint32(spec.seed));
    const double swingDelay = std::clamp(spec.swing, 0.0, 100.0) / 100.0 * step / 3.0;

    for (int seg = 0; seg < segments; ++seg) {
        const auto& tones = chordTones[seg % chordTones.size()];
        const double segStart = seg * spec.beatsPerChord;
        const double segEnd = std::min(segStart + spec.beatsPerChord, total);
        const int stepCount = int(std::floor((segEnd - segStart) / step + 1e-9));
        const QVector<int> order = toneOrder(spec, tones.size(), stepCount, rng);

        for (int i = 0; i < stepCount; ++i) {
            double start = segStart + i * step;
            const bool onBeat = std::abs(start - std::round(start)) < 1e-6;
            if (i % 2 == 1) start += swingDelay;
            const int velocity = std::clamp(spec.velocity + (onBeat ? spec.accent : 0), 1, 127);
            const double length = std::max(0.01, step * std::clamp(spec.gate, 0.05, 1.0));

            QVector<int> pitches;
            if (spec.steps.isEmpty() && spec.pattern == "chord") {
                pitches = tones;
            } else {
                // Indices past the chord's tones continue into higher octaves.
                const int index = order[i % order.size()];
                const int n = tones.size();
                const int octaveShift = index >= 0 ? index / n : -((-index - 1) / n + 1);
                pitches = { tones[((index % n) + n) % n] + 12 * octaveShift };
            }

            for (int pitch : pitches) {
                if (pitch < 0 || pitch > 127) continue;
                result.notes.append({ pitch, start, length, velocity });
            }
        }
    }
    return result;
}

} // namespace OpenDaw::Patterns
