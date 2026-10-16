#include "DrumPatterns.h"
#include <QMap>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenDaw::DrumPatterns {

namespace {

constexpr double kStep = 0.25;   // the groove grid is in 16ths
constexpr double kEps = 1e-6;

// Intensity bands: grooves gain layers as intensity crosses these.
constexpr double M = 0.34;       // medium
constexpr double H = 0.67;       // high
constexpr double T = 0.85;       // top
constexpr double X = 2.0;        // no upper bound

// Variant masks; 0 = every variant.
constexpr unsigned ALL = 0, V0 = 1, V1 = 2, V2 = 4;

enum class Fill { TomRun, SnareToms, SnareRoll, Light, Triplet, DanceBuild, TrapRoll };

// One instrument line of a groove. `steps` is one 16th per character and
// repeats to fill the bar; spaces are only for readability.
//   X accent, x normal, o medium, m soft, g ghost, f feathered,
//   r two 32nds, t three triplets spread over two 16ths (the next step is '.').
struct Layer {
    int pitch;
    const char* steps;
    double minIntensity;
    double maxIntensity;    // exclusive
    unsigned variants;
};

struct StyleDef {
    const char* name;
    const char* description;
    int beatsPerBar;
    double defaultSwing;
    int tempoMin, tempoMax;
    double swingUnit;       // the subdivision swing delays: 0.5 = 8ths, 0.25 = 16ths
    int variantCount;
    double dynamics;        // overall velocity scale
    std::vector<Fill> fills;
    int fillSnare;          // the drum fills use as their snare
    bool flams;
    bool doubleKickFills;
    std::vector<Layer> layers;
};

const std::vector<StyleDef>& styleDefs()
{
    static const std::vector<StyleDef> defs = {
        { "rock", "Straight 8th-note rock: kick on 1 and 3, backbeat snare on 2 and 4.",
          4, 0, 90, 150, 0.5, 3, 1.0, { Fill::SnareToms, Fill::TomRun, Fill::SnareRoll }, kSnare, true, false, {
            { kKick, "x... .... x... ....", 0, X, V0 },
            { kKick, ".... .... ..x. ....", M, X, V0 },
            { kKick, ".... .... .... ...x", H, X, V0 },
            { kKick, "x... .... x.x. ....", 0, X, V1 },
            { kKick, ".... ...x .... ....", M, X, V1 },
            { kKick, ".... .... .... ..x.", H, X, V1 },
            { kKick, "x.x. .... x... ....", 0, X, V2 },
            { kKick, ".... .... ..x. ....", M, X, V2 },
            { kKick, ".... ...x .... ....", H, X, V2 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, ".... ...g .g.. ...g", H, X, ALL },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "xmom", H, X, V0 | V1 },
            { kOpenHat, ".... .... .... ..x.", T, X, V0 | V1 },
            { kRide, "x.o.", H, X, V2 },
            { kPedalHat, ".... x... .... x...", H, X, V2 },
        } },
        { "pop", "Clean pop backbeat with snare and clap on 2 and 4, tambourine on top.",
          4, 0, 90, 130, 0.5, 3, 0.95, { Fill::Light, Fill::SnareToms }, kSnare, false, false, {
            { kKick, "x... .... x.x. ....", 0, X, V0 },
            { kKick, "x... ..x. x... ....", 0, X, V1 },
            { kKick, "x..x ..x. x... ....", 0, X, V2 },
            { kKick, ".... .... .... ..x.", H, X, ALL },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kClap, ".... x... .... x...", M, X, ALL },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "xmom", H, X, ALL },
            { kTambourine, "..x.", H, X, V0 | V2 },
            { kTambourine, ".... x... .... x...", H, X, V1 },
            { kOpenHat, ".... .... .... ..x.", T, X, ALL },
        } },
        { "blues_shuffle", "Triplet shuffle: swung hi-hat or ride, kick on 1 and 3 (or all four), backbeat.",
          4, 100, 70, 140, 0.5, 2, 0.95, { Fill::Triplet }, kSnare, false, false, {
            { kClosedHat, "x.o.", 0, X, V0 },
            { kRide, "x.o.", 0, X, V1 },
            { kPedalHat, ".... x... .... x...", M, X, V1 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, "..g. .... ..g. ....", M, X, ALL },
            { kSnare, ".... ..g. .... ..g.", H, X, ALL },
            { kKick, "x... .... x... ....", 0, X, V0 },
            { kKick, ".... ..x. .... ..x.", M, X, V0 },
            { kKick, "x...", 0, X, V1 },
            { kKick, ".... .... .... ..x.", M, X, V1 },
            { kOpenHat, ".... .... .... ..x.", T, X, V0 },
        } },
        { "funk", "Syncopated 16th-note funk with ghost notes on the snare and open hi-hat accents.",
          4, 0, 90, 120, 0.25, 3, 1.0, { Fill::SnareToms, Fill::TomRun }, kSnare, false, false, {
            { kKick, "x... .... ..x. ....", 0, X, V0 },
            { kKick, ".... ...x .... .x..", M, X, V0 },
            { kKick, "..x. .... .... ....", H, X, V0 },
            { kKick, "x..x .... ..x. ....", 0, X, V1 },
            { kKick, ".... .... .... ..x.", M, X, V1 },
            { kKick, ".... ..x. ...x ....", H, X, V1 },
            { kKick, "x.x. .... .x.. ....", 0, X, V2 },
            { kKick, ".... ...x .... ..x.", M, X, V2 },
            { kKick, ".... .... ...x ...x", H, X, V2 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, ".g.. ...g .g.. ....", M, X, ALL },
            { kSnare, "...g .g.. ...g .g.g", H, X, ALL },
            { kClosedHat, "x.o.", 0, M, ALL },
            { kClosedHat, "xmom", M, X, ALL },
            { kOpenHat, ".... .... .... ..x.", H, X, V0 },
            { kOpenHat, ".... ..x. .... ..x.", H, X, V1 },
            { kOpenHat, "..x. .... ..x. ....", H, X, V2 },
        } },
        { "hiphop", "Boom bap: heavy kick, cracking snare on 2 and 4, lazily swung 16th hats.",
          4, 50, 80, 100, 0.25, 3, 1.0, { Fill::SnareRoll, Fill::SnareToms }, kSnare, false, false, {
            { kKick, "x... .... ..x. ....", 0, X, V0 },
            { kKick, "...x .... .... ....", M, X, V0 },
            { kKick, ".... .... ...x ....", H, X, V0 },
            { kKick, "x..x .... x... ....", 0, X, V1 },
            { kKick, ".... .... ..x. ....", M, X, V1 },
            { kKick, ".... ...x .... ..x.", H, X, V1 },
            { kKick, "x... ...x ..x. ....", 0, X, V2 },
            { kKick, ".... .... .... .x..", M, X, V2 },
            { kKick, "..x. .... .... ....", H, X, V2 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, ".... ...g .... .g..", H, X, ALL },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "x.om", H, X, ALL },
            { kOpenHat, ".... .... .... ..x.", T, X, ALL },
        } },
        { "trap", "Half-time trap: clap on 3, sparse 808 kicks, 16th hats with triplet and 32nd rolls.",
          4, 0, 130, 170, 0.25, 2, 1.0, { Fill::TrapRoll }, kSnare, false, false, {
            { kKick, "x... .... .... ..x.", 0, X, V0 },
            { kKick, ".... ..x. .... ....", M, X, V0 },
            { kKick, "...x .... ...x ....", H, X, V0 },
            { kKick, "x..x .... .... ....", 0, X, V1 },
            { kKick, ".... .... ..x. .x..", M, X, V1 },
            { kKick, ".... ...x .... ...x", H, X, V1 },
            { kClap, ".... .... X... ....", 0, X, ALL },
            { kSnare, ".... .... x... ....", M, X, ALL },
            { kClosedHat, "x.o.", 0, M, ALL },
            { kClosedHat, "xmom", M, H, ALL },
            { kClosedHat, "xmom xmom xmom t.t.", H, X, V0 },
            { kClosedHat, "xmom t.t. xmom xmrr", H, X, V1 },
        } },
        { "house", "Four-on-the-floor house: kick every beat, clap on 2 and 4, open hats on the off-beats.",
          4, 0, 118, 130, 0.25, 2, 1.0, { Fill::DanceBuild }, kSnare, false, false, {
            { kKick, "x...", 0, X, ALL },
            { kClap, ".... x... .... x...", 0, X, ALL },
            { kClap, ".... .... .... ...m", H, X, V1 },
            { kClosedHat, "..x.", 0, M, ALL },
            { kOpenHat, "..x.", M, X, ALL },
            { kClosedHat, ".m.m", M, X, V0 },
            { kClosedHat, "m..m", M, X, V1 },
            { kMaracas, "omom", H, X, ALL },
        } },
        { "disco", "Disco: four-on-the-floor kick, snare on 2 and 4, open hi-hat on every off-beat.",
          4, 0, 110, 130, 0.25, 2, 1.0, { Fill::DanceBuild }, kSnare, false, false, {
            { kKick, "x...", 0, X, ALL },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kClosedHat, "x...", 0, M, ALL },
            { kOpenHat, "..x.", M, X, ALL },
            { kClosedHat, "x...", M, H, V0 },
            { kClosedHat, "xm.m", H, X, V0 },
            { kClosedHat, "xm.m", M, X, V1 },
            { kClap, ".... x... .... x...", H, X, V0 },
            { kTambourine, ".... x... .... x...", H, X, V1 },
        } },
        { "reggae", "One drop: nothing on 1, kick and rim click together on 3, lightly swung hats.",
          4, 50, 60, 90, 0.5, 2, 0.95, { Fill::Triplet, Fill::TomRun }, kSnare, false, false, {
            { kKick, ".... .... x... ....", 0, X, ALL },
            { kSideStick, ".... .... X... ....", 0, X, V0 },
            { kSideStick, ".... .... X... ..o.", 0, X, V1 },
            { kSideStick, ".g.. .... .... .g..", H, X, ALL },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "xmom", H, X, V0 },
            { kClosedHat, "x.o.", H, X, V1 },
            { kOpenHat, ".... ..x. .... ..x.", H, X, V1 },
        } },
        { "bossa_nova", "Bossa nova: soft bass drum on 1 and the 'a' of 1, rim-click clave, light 8th hats.",
          4, 0, 110, 150, 0.25, 2, 0.8, { Fill::Light }, kSideStick, false, false, {
            { kKick, "x... .... x... ....", 0, X, ALL },
            { kKick, "...x x..x ...x x..x", M, X, ALL },
            { kSideStick, "x..x ..x. ..x. .x..", 0, X, V0 },
            { kSideStick, "..x. .x.. x..x ..x.", 0, X, V1 },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, X, ALL },
            { kMaracas, "omom", H, X, ALL },
        } },
        { "jazz_swing", "Swing: spang-a-lang ride, feathered quarter-note kick, hi-hat foot on 2 and 4, snare comping.",
          4, 80, 80, 240, 0.5, 3, 0.85, { Fill::Triplet }, kSnare, false, false, {
            { kRide, "x...", 0, M, ALL },
            { kRide, "x... x.o. x... x.o.", M, X, ALL },
            { kPedalHat, ".... x... .... x...", 0, X, ALL },
            { kKick, "f... .... f... ....", 0, X, ALL },
            { kKick, ".... f... .... f...", M, X, ALL },
            { kSnare, ".... .... .... ..m.", M, X, V0 },
            { kSnare, ".... ..m. .... ....", M, X, V1 },
            { kSnare, "..m. .... .... ....", M, X, V2 },
            { kSnare, ".... ..g. ..m. ....", H, X, V0 },
            { kSnare, "..g. .... ..m. ..g.", H, X, V1 },
            { kSnare, ".... ..m. .... ..g.", H, X, V2 },
            { kKick, ".... .... .... ..x.", H, X, V0 },
            { kKick, ".... ..x. .... ....", H, X, V1 },
            { kKick, "..x. .... .... ....", H, X, V2 },
        } },
        { "waltz", "Waltz in 3/4: kick on 1, soft snare on 2 and 3.",
          3, 0, 84, 180, 0.5, 2, 0.85, { Fill::Light, Fill::TomRun }, kSnare, false, false, {
            { kKick, "x... .... ....", 0, X, ALL },
            { kKick, "..x. .... ....", H, X, V0 },
            { kKick, ".... .... ..x.", M, X, V1 },
            { kSnare, ".... o... o...", 0, X, V0 },
            { kSideStick, ".... x... x...", 0, X, V1 },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "xmom", H, X, ALL },
        } },
        { "metal", "Metal: driving 8th kicks rising to 16th double kick, hard backbeat, ride or china on top.",
          4, 0, 100, 200, 0.25, 2, 1.05, { Fill::TomRun, Fill::SnareToms }, kSnare, true, true, {
            { kKick, "x... ..x. x.x. ....", 0, M, V0 },
            { kKick, "x.x.", M, H, V0 },
            { kKick, "x...", 0, M, V1 },
            { kKick, "x.xx", M, H, V1 },
            { kKick, "xxxx", H, X, ALL },
            { kSnare, ".... X... .... X...", 0, X, V0 },
            { kSnare, ".... .... X... ....", 0, X, V1 },
            { kClosedHat, "x.o.", 0, H, V0 },
            { kRide, "x.o.", H, X, V0 },
            { kClosedHat, "x...", 0, M, V1 },
            { kClosedHat, "x.o.", M, H, V1 },
            { kChina, "x...", H, X, V1 },
            { kRide, "..o.", H, X, V1 },
        } },
        { "drum_and_bass", "Drum and bass two-step: kick on 1 and the '&' of 3, snare on 2 and 4, fast hats.",
          4, 0, 160, 180, 0.25, 2, 1.0, { Fill::SnareRoll, Fill::SnareToms }, kSnare, false, false, {
            { kKick, "x... .... ..x. ....", 0, X, V0 },
            { kKick, "...x .... .... ....", H, X, V0 },
            { kKick, "x.x. .... ..x. ....", 0, X, V1 },
            { kKick, ".... ...x .... ....", M, X, V1 },
            { kKick, ".... .... .... .x..", H, X, V1 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, ".... ...o .o.. ....", M, X, V1 },
            { kSnare, ".... ...g .g.. ..g.", H, X, V0 },
            { kSnare, "..g. .... .... ..g.", H, X, V1 },
            { kClosedHat, "x...", 0, M, ALL },
            { kClosedHat, "x.o.", M, H, ALL },
            { kClosedHat, "xmom", H, X, ALL },
            { kOpenHat, ".... .... ..x. ....", T, X, ALL },
        } },
        { "afrobeat", "Afrobeat: busy 16th hats with open accents, syncopated kick, ghosted snare, cowbell.",
          4, 0, 100, 130, 0.25, 2, 1.0, { Fill::TomRun, Fill::SnareToms }, kSnare, false, false, {
            { kKick, "x... .... ..x. ....", 0, X, V0 },
            { kKick, ".... ...x .... ....", M, X, V0 },
            { kKick, ".... .... .... ..x.", H, X, V0 },
            { kKick, "x... ..x. .... x...", 0, X, V1 },
            { kKick, "...x .... .... ....", M, X, V1 },
            { kKick, ".... .... ..x. ...x", H, X, V1 },
            { kSnare, ".... X... .... X...", 0, X, ALL },
            { kSnare, "..g. ..g. .g.. ..g.", M, X, ALL },
            { kSnare, ".g.. .... ...g ....", H, X, ALL },
            { kClosedHat, "x.o.", 0, M, ALL },
            { kClosedHat, "xmom", M, X, ALL },
            { kOpenHat, "...x .... ...x ....", M, X, V0 },
            { kOpenHat, ".... ..x. .... ..x.", M, X, V1 },
            { kCowbell, "x.x. .x.x x.x. .x.x", H, X, ALL },
        } },
    };
    return defs;
}

const StyleDef* findStyle(const QString& name)
{
    static const QMap<QString, QString> aliases = {
        { "hip_hop", "hiphop" }, { "boom_bap", "hiphop" }, { "shuffle", "blues_shuffle" },
        { "blues", "blues_shuffle" }, { "jazz", "jazz_swing" }, { "swing", "jazz_swing" },
        { "bossa", "bossa_nova" }, { "dnb", "drum_and_bass" }, { "one_drop", "reggae" },
        { "afro", "afrobeat" },
    };
    QString key = name.trimmed().toLower();
    key.replace(' ', '_').replace('-', '_');
    key = aliases.value(key, key);
    for (const auto& def : styleDefs())
        if (key == def.name) return &def;
    return nullptr;
}

// One hit before timing is finalised: `pos` is in beats from the bar start.
struct Hit {
    int pitch;
    double pos;
    double velocity;    // before the intensity/dynamics scale
    bool swing;         // false for triplet fills, which are placed in straight time
};

double charVelocity(char c)
{
    switch (c) {
    case 'X': return 118;
    case 'x': return 100;
    case 'o': return 84;
    case 'm': return 68;
    case 'f': return 46;
    case 'g': return 38;
    case 'r': return 72;
    case 't': return 76;
    default: return 0;
    }
}

void addLayer(const Layer& layer, int barSteps, QVector<Hit>& out)
{
    QByteArray steps(layer.steps);
    steps.replace(' ', "");
    if (steps.isEmpty()) return;
    for (int i = 0; i < barSteps; ++i) {
        const char c = steps[i % steps.size()];
        const double v = charVelocity(c);
        if (v <= 0) continue;
        const double pos = i * kStep;
        if (c == 'r') {
            out.append({ layer.pitch, pos, v, true });
            out.append({ layer.pitch, pos + kStep / 2, v * 0.85, true });
        } else if (c == 't') {
            for (int k = 0; k < 3; ++k)
                if (pos + k * 2 * kStep / 3 < barSteps * kStep - kEps)
                    out.append({ layer.pitch, pos + k * 2 * kStep / 3, v - 8 * k, true });
        } else {
            out.append({ layer.pitch, pos, v, true });
        }
    }
}

quint64 mix(quint64 x)
{
    // splitmix64 finaliser: decorrelates nearby inputs.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

quint64 hash(std::initializer_list<qint64> parts)
{
    quint64 h = 0x5DEECE66Dull;
    for (qint64 p : parts) h = mix(h ^ quint64(p));
    return h;
}

double unit(quint64 h) { return double(h >> 11) / double(1ull << 53); }

// Toms from high to low, for runs around the kit.
const int kToms[] = { kHighTom, kHighMidTom, kLowMidTom, kLowTom, kHighFloorTom, kLowFloorTom };

int tomAt(int index, int count)
{
    return kToms[std::min(5, index * 6 / std::max(1, count))];
}

// Fills the region [start, start + length) of a bar.
void addFill(const StyleDef& style, Fill kind, double start, double length, double intensity,
             double flamOffset, QVector<Hit>& out)
{
    const bool triplet = kind == Fill::Triplet;
    const int perBeat = kind == Fill::Light ? (intensity < H ? 2 : 4)
                      : triplet ? (intensity < H ? 3 : 6)
                      : (intensity < M ? 2 : 4);
    const int count = std::max(1, int(std::lround(length * perBeat)));
    auto posOf = [&](int k) { return start + double(k) / perBeat; };
    auto ramp = [&](int k, double from, double to) {
        return count > 1 ? from + (to - from) * k / (count - 1) : to;
    };
    auto hit = [&](int pitch, double pos, double velocity) {
        out.append({ pitch, pos, velocity, !triplet });
    };

    int firstPitch = -1;
    switch (kind) {
    case Fill::TomRun:
        for (int k = 0; k < count; ++k)
            hit(tomAt(k, count), posOf(k), ramp(k, 92, 116));
        hit(kKick, posOf(count - 1), 104);
        firstPitch = tomAt(0, count);
        break;
    case Fill::SnareToms: {
        const int snares = count / 2;
        for (int k = 0; k < count; ++k) {
            if (k < snares) hit(style.fillSnare, posOf(k), ramp(k, 86, 110));
            else hit(tomAt(k - snares, count - snares), posOf(k), ramp(k, 92, 116));
        }
        hit(kKick, posOf(count - 1), 104);
        firstPitch = snares > 0 ? style.fillSnare : tomAt(0, count);
        break;
    }
    case Fill::SnareRoll:
        for (int k = 0; k < count; ++k)
            hit(style.fillSnare, posOf(k), ramp(k, 64, 118));
        hit(kKick, posOf(count - 1), 100);
        firstPitch = style.fillSnare;
        break;
    case Fill::Light:
        // A gentle pickup: snare and high tom alternating, settling on a low tom.
        for (int k = 0; k < count; ++k) {
            const int pitch = k == count - 1 && count > 1 ? kLowTom
                            : (k % 2 == 0 ? style.fillSnare : kHighMidTom);
            hit(pitch, posOf(k), ramp(k, 70, 92));
        }
        break;
    case Fill::Triplet:
        // Snare, tom, kick on each triplet, the tom stepping down the kit.
        for (int k = 0; k < count; ++k) {
            const int pitch = k % 3 == 0 ? style.fillSnare : k % 3 == 1 ? tomAt(k, count) : kKick;
            hit(pitch, posOf(k), ramp(k, 72, 108));
        }
        break;
    case Fill::DanceBuild:
        for (int k = 0; k < count; ++k)
            hit(style.fillSnare, posOf(k), ramp(k, 58, 118));
        if (intensity >= H) hit(kClap, posOf(count - 1), 110);
        break;
    case Fill::TrapRoll: {
        const int hatsPerBeat = intensity < M ? 4 : 8;
        const int hats = std::max(1, int(std::lround(length * hatsPerBeat)));
        for (int k = 0; k < hats; ++k)
            hit(kClosedHat, start + double(k) / hatsPerBeat, 50 + 50.0 * k / std::max(1, hats - 1));
        for (int k = 0; k < count; ++k)
            hit(style.fillSnare, posOf(k), ramp(k, 70, 112));
        break;
    }
    }

    if (style.doubleKickFills && intensity >= H)
        for (int k = 0; k < int(std::lround(length * 4)); ++k)
            hit(kKick, start + k * kStep, 96);

    // A flam: a soft grace stroke just ahead of the fill's first hit.
    if (style.flams && firstPitch >= 0 && flamOffset > 0 && start - flamOffset >= 0)
        hit(firstPitch, start - flamOffset, 46);
}

// Delays every second `unit` towards the triplet position; amount 1 = full triplet.
double swingWarp(double pos, double unit, double amount)
{
    const double cell = 2 * unit;
    const double c = std::floor(pos / cell + 1e-9);
    const double r = pos - c * cell;
    const double d = amount * unit / 3;
    const double warped = r <= unit ? r * (unit + d) / unit
                                    : unit + d + (r - unit) * (unit - d) / unit;
    return c * cell + warped;
}

bool isHat(int pitch) { return pitch == kClosedHat || pitch == kPedalHat || pitch == kOpenHat; }

// Hats share one group so humanize never lets a closed and an open hat meet.
int timingGroup(int pitch) { return isHat(pitch) ? kClosedHat : pitch; }

struct Placed {
    int pitch;
    double start;
    double velocity;
};

QString fillName(Fill f)
{
    switch (f) {
    case Fill::TomRun: return "tom run";
    case Fill::SnareToms: return "snare and toms";
    case Fill::SnareRoll: return "snare roll";
    case Fill::Light: return "light pickup";
    case Fill::Triplet: return "triplet fill";
    case Fill::DanceBuild: return "snare build";
    case Fill::TrapRoll: return "hat and snare roll";
    }
    return {};
}

} // namespace

QVector<StyleInfo> styles()
{
    QVector<StyleInfo> list;
    for (const auto& def : styleDefs())
        list.append({ def.name, def.description, def.beatsPerBar, def.defaultSwing,
                      def.tempoMin, def.tempoMax });
    return list;
}

Result generate(const Spec& spec)
{
    Result result;
    const StyleDef* style = findStyle(spec.style);
    if (!style) {
        QStringList names;
        for (const auto& def : styleDefs()) names << def.name;
        result.error = QString("Unknown style '%1'. Use %2.").arg(spec.style, names.join(", "));
        return result;
    }
    if (spec.bars < 1 || spec.bars > 1024) {
        result.error = QString("bars must be between 1 and 1024 (got %1).").arg(spec.bars);
        return result;
    }
    if (spec.fillEvery < 0) {
        result.error = QString("fillEvery must be 0 (no fills) or a number of bars (got %1).")
                           .arg(spec.fillEvery);
        return result;
    }
    if (spec.humanize > 0 && !(spec.tempo > 0)) {
        result.error = QString("tempo must be positive to humanize (got %1).").arg(spec.tempo);
        return result;
    }

    const int bpb = style->beatsPerBar;
    const int barSteps = bpb * 4;
    const double total = double(spec.bars) * bpb;
    const double intensity = std::clamp(spec.intensity, 0.0, 1.0);
    const double swing = spec.swing < 0 ? style->defaultSwing : std::min(spec.swing, 100.0);
    const int variant = ((spec.variation % style->variantCount) + style->variantCount)
                        % style->variantCount;
    const double humanize = std::clamp(spec.humanize, 0.0, 1.0);
    const double tempo = spec.tempo > 0 ? spec.tempo : 120.0;
    // Fills snap to the 16th grid and fit inside one bar.
    const double fillBeats = spec.fillBeats > 0
        ? std::clamp(std::round(spec.fillBeats / kStep) * kStep, kStep, double(bpb)) : 0.0;
    const bool fills = spec.fillEvery > 0 && fillBeats > 0;
    auto isFillBar = [&](int bar) {
        return fills && ((bar + 1) % spec.fillEvery == 0 || bar == spec.bars - 1);
    };
    // About 25 ms, a typical flam spread, but never more than half a 16th.
    const double flamOffset = std::min(0.025 * tempo / 60.0, kStep / 2);

    QVector<Hit> groove;
    for (const auto& layer : style->layers) {
        if (intensity < layer.minIntensity || intensity >= layer.maxIntensity) continue;
        if (layer.variants != ALL && !(layer.variants & (1u << variant))) continue;
        addLayer(layer, barSteps, groove);
    }

    QVector<Placed> placed;
    int fillCount = 0;
    for (int bar = 0; bar < spec.bars; ++bar) {
        QVector<Hit> hits = groove;
        const bool fillBar = isFillBar(bar);
        if (fillBar) {
            const double fillStart = bpb - fillBeats;
            const Fill kind = style->fills[size_t(hash({ spec.seed, spec.variation, bar })
                                                  % style->fills.size())];
            const bool keepKick = kind == Fill::DanceBuild || kind == Fill::TrapRoll;
            hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const Hit& h) {
                           return h.pos >= fillStart - kEps && !(keepKick && h.pitch == kKick);
                       }), hits.end());
            addFill(*style, kind, fillStart, fillBeats, intensity, flamOffset, hits);
            ++fillCount;
        }
        if ((bar == 0 && spec.crashOnFirst) || (bar > 0 && isFillBar(bar - 1))) {
            // The crash takes over from the downbeat's timekeeping cymbal.
            hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Hit& h) {
                           return std::abs(h.pos) < kEps
                               && (isHat(h.pitch) || h.pitch == kRide || h.pitch == kChina);
                       }), hits.end());
            hits.append({ kCrash, 0.0, 116, true });
        }
        for (const auto& h : hits) {
            const double pos = h.swing && swing > 0
                ? swingWarp(h.pos, style->swingUnit, swing / 100.0) : h.pos;
            placed.append({ h.pitch, bar * bpb + pos, h.velocity });
        }
    }

    // Merge hits that land together: one note per drum, and an open hat chokes
    // a closed or pedal hat struck with it.
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
        return a.start < b.start - kEps || (std::abs(a.start - b.start) <= kEps && a.pitch < b.pitch);
    });
    QVector<Placed> merged;
    for (int i = 0; i < placed.size();) {
        int j = i;
        while (j < placed.size() && placed[j].start - placed[i].start <= kEps) ++j;
        bool open = false;
        for (int k = i; k < j; ++k) open |= placed[k].pitch == kOpenHat;
        for (int k = i; k < j; ++k) {
            const Placed& p = placed[k];
            if (open && (p.pitch == kClosedHat || p.pitch == kPedalHat)) continue;
            if (!merged.isEmpty() && merged.last().pitch == p.pitch
                && p.start - merged.last().start <= kEps) {
                merged.last().velocity = std::max(merged.last().velocity, p.velocity);
                continue;
            }
            merged.append({ p.pitch, placed[i].start, p.velocity });
        }
        i = j;
    }

    // Humanize: each note may drift at most 10 ms (at full humanize), and at most
    // 40% of the way to its neighbours on the same drum so strokes never meet.
    const double maxShift = humanize * 0.010 * tempo / 60.0;
    const double velocityScale = style->dynamics * (0.8 + 0.3 * intensity);
    QVector<Patterns::Note> notes;
    notes.reserve(merged.size());
    for (int i = 0; i < merged.size(); ++i) {
        const Placed& p = merged[i];
        double start = p.start;
        double velocity = p.velocity * velocityScale;
        if (humanize > 0) {
            double limit = maxShift;
            const int group = timingGroup(p.pitch);
            for (int k = i - 1; k >= 0; --k)
                if (timingGroup(merged[k].pitch) == group) {
                    limit = std::min(limit, 0.4 * (start - merged[k].start));
                    break;
                }
            for (int k = i + 1; k < merged.size(); ++k)
                if (timingGroup(merged[k].pitch) == group) {
                    limit = std::min(limit, 0.4 * (merged[k].start - start));
                    break;
                }
            const qint64 tick = std::llround(p.start * 960);
            // Triangular jitter: small drifts are more likely than large ones.
            const double t = unit(hash({ spec.seed, p.pitch, tick, 1 }))
                           - unit(hash({ spec.seed, p.pitch, tick, 2 }));
            const double v = unit(hash({ spec.seed, p.pitch, tick, 3 }))
                           - unit(hash({ spec.seed, p.pitch, tick, 4 }));
            start = std::clamp(start + t * limit, 0.0, total - 1.0 / 64);
            velocity += v * 12 * humanize;
        }
        notes.append({ p.pitch, start, 0.25, std::clamp(int(std::lround(velocity)), 1, 127) });
    }
    std::sort(notes.begin(), notes.end(), [](const Patterns::Note& a, const Patterns::Note& b) {
        return a.startBeat < b.startBeat || (!(b.startBeat < a.startBeat) && a.pitch < b.pitch);
    });

    // Drum hits are one-shots; keep each note short and clear of the next
    // stroke on the same drum and of the pattern end.
    for (int i = 0; i < notes.size(); ++i) {
        double length = std::min(notes[i].lengthBeats, total - notes[i].startBeat);
        for (int k = i + 1; k < notes.size(); ++k)
            if (notes[k].pitch == notes[i].pitch) {
                length = std::min(length, notes[k].startBeat - notes[i].startBeat);
                break;
            }
        notes[i].lengthBeats = length;
    }

    QStringList fillKinds;
    for (Fill f : style->fills) fillKinds << fillName(f);
    const QString fillText = fillCount == 0
        ? QString("no fills")
        : QString("%1 fill%2 of %3 beat%4 (%5)")
              .arg(fillCount).arg(fillCount == 1 ? "" : "s")
              .arg(fillBeats).arg(std::abs(fillBeats - 1.0) < kEps ? "" : "s")
              .arg(fillKinds.join(" / "));
    const QString swingText = swing > 0 ? QString("swing %1").arg(swing, 0, 'f', 0)
                                        : QString("straight");

    result.notes = notes;
    result.summary = QString("%1, %2 bar%3 of %4/4, intensity %5, %6, variation %7 (0-%8), "
                             "%9, %10 notes.")
                         .arg(style->name)
                         .arg(spec.bars).arg(spec.bars == 1 ? "" : "s").arg(bpb)
                         .arg(intensity, 0, 'f', 2)
                         .arg(swingText)
                         .arg(variant).arg(style->variantCount - 1)
                         .arg(fillText)
                         .arg(notes.size());
    return result;
}

} // namespace OpenDaw::DrumPatterns
