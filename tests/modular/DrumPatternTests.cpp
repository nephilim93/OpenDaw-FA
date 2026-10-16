#include "TestHarness.h"
#include "music/DrumPatterns.h"
#include <QMap>
#include <QSet>
#include <algorithm>

using namespace OpenDaw::Tests;
namespace DP = OpenDaw::DrumPatterns;
using OpenDaw::Patterns::Note;

namespace {

// Grid positions are sums of exact binary fractions or thirds, so anything
// meant to coincide agrees to rounding error; 1e-9 beats is far below a MIDI tick.
constexpr double kTol = 1e-9;

DP::Result gen(const DP::Spec& spec)
{
    auto r = DP::generate(spec);
    CHECK_MSG(r.error.isEmpty(), spec.style + ": " + r.error);
    return r;
}

std::string label(const DP::Spec& s)
{
    return str(s.style) + " i=" + str(s.intensity) + " v=" + str(double(s.variation))
         + " seed=" + str(double(s.seed)) + " h=" + str(s.humanize);
}

bool isTom(int p)
{
    return p == DP::kLowFloorTom || p == DP::kHighFloorTom || p == DP::kLowTom
        || p == DP::kLowMidTom || p == DP::kHighMidTom || p == DP::kHighTom;
}

bool isSnareFamily(int p)
{
    return p == DP::kSideStick || p == DP::kSnare || p == DP::kClap || p == DP::kElectricSnare;
}

bool isCrash(int p) { return p == DP::kCrash || p == DP::kCrash2; }

int countAt(const QVector<Note>& notes, int pitch, double beat)
{
    return int(std::count_if(notes.begin(), notes.end(), [&](const Note& n) {
        return n.pitch == pitch && std::abs(n.startBeat - beat) <= kTol;
    }));
}

bool hasAt(const QVector<Note>& notes, int pitch, double beat) { return countAt(notes, pitch, beat) > 0; }

template <typename Pred>
int countIn(const QVector<Note>& notes, double from, double to, Pred pred)
{
    return int(std::count_if(notes.begin(), notes.end(), [&](const Note& n) {
        return n.startBeat >= from - kTol && n.startBeat < to - kTol && pred(n.pitch);
    }));
}

bool sameNotes(const QVector<Note>& a, const QVector<Note>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].pitch != b[i].pitch || a[i].startBeat != b[i].startBeat
            || a[i].velocity != b[i].velocity || a[i].lengthBeats != b[i].lengthBeats)
            return false;
    return true;
}

int beatsPerBar(const QString& style)
{
    for (const auto& s : DP::styles())
        if (s.name == style) return s.beatsPerBar;
    return 0;
}

// The rules every pattern must keep, whatever the style or settings.
void checkInvariants(const DP::Result& r, double totalBeats, const std::string& what)
{
    CHECK_MSG(!r.notes.isEmpty(), what);
    for (int i = 0; i < r.notes.size(); ++i) {
        const Note& n = r.notes[i];
        CHECK_MSG(n.velocity >= 1 && n.velocity <= 127, what + " velocity " + str(double(n.velocity)));
        CHECK_MSG(n.startBeat >= 0.0, what + " starts before 0: " + str(n.startBeat));
        CHECK_MSG(n.lengthBeats > 0.0, what + " empty note at " + str(n.startBeat));
        CHECK_MSG(n.startBeat + n.lengthBeats <= totalBeats + kTol,
                  what + " ends past the pattern: " + str(n.startBeat + n.lengthBeats));
        if (i > 0) CHECK_MSG(r.notes[i - 1].startBeat <= n.startBeat, what + " not sorted");
    }
    // Same-pitch and open/closed hat collisions: compare every pair that starts
    // within kTol (the list is sorted, so only neighbours need checking).
    for (int i = 0; i < r.notes.size(); ++i)
        for (int j = i + 1; j < r.notes.size() && r.notes[j].startBeat - r.notes[i].startBeat <= kTol; ++j) {
            const int a = r.notes[i].pitch, b = r.notes[j].pitch;
            CHECK_MSG(a != b, what + " two notes of pitch " + str(double(a)) + " at " + str(r.notes[i].startBeat));
            const bool closedOpen = (a == DP::kOpenHat && (b == DP::kClosedHat || b == DP::kPedalHat))
                                 || (b == DP::kOpenHat && (a == DP::kClosedHat || a == DP::kPedalHat));
            CHECK_MSG(!closedOpen, what + " closed and open hat together at " + str(r.notes[i].startBeat));
        }
}

// Fractional beat of a time, in [0, 1).
double frac(double beat) { return beat - std::floor(beat + kTol); }

} // namespace

TEST(drumpatterns, style_list)
{
    const QStringList required = { "rock", "pop", "blues_shuffle", "funk", "hiphop", "trap", "house",
                                   "disco", "reggae", "bossa_nova", "jazz_swing", "waltz", "metal",
                                   "drum_and_bass", "afrobeat" };
    QStringList names;
    for (const auto& s : DP::styles()) {
        names << s.name;
        CHECK_MSG(!s.description.isEmpty(), s.name);
        CHECK_MSG(s.beatsPerBar == 3 || s.beatsPerBar == 4, s.name);
        CHECK_MSG(s.defaultSwing >= 0 && s.defaultSwing <= 100, s.name);
        CHECK_MSG(s.tempoMin > 0 && s.tempoMin < s.tempoMax, s.name);
    }
    for (const auto& name : required) CHECK_MSG(names.contains(name), name);

    DP::Spec spec;
    spec.style = "polka-step";
    const auto r = DP::generate(spec);
    CHECK(r.notes.isEmpty());
    CHECK(r.error.contains("polka-step"));
    for (const auto& name : names) CHECK_MSG(r.error.contains(name), r.error);

    // Names are forgiving about case and separators.
    spec.style = "Bossa Nova";
    CHECK(DP::generate(spec).error.isEmpty());
}

TEST(drumpatterns, invariants_every_style)
{
    for (const auto& style : DP::styles())
        for (double intensity : { 0.0, 0.5, 1.0 })
            for (int seed : { 1, 2, 7, 1234 })
                for (int variation : { 0, 1, 2 })
                    for (double humanize : { 0.0, 1.0 })
                        for (double fillBeats : { 1.0, 2.0 }) {
                            DP::Spec spec;
                            spec.style = style.name;
                            spec.bars = 8;
                            spec.intensity = intensity;
                            spec.seed = seed;
                            spec.variation = variation;
                            spec.humanize = humanize;
                            spec.fillBeats = fillBeats;
                            spec.tempo = style.tempoMax;  // the widest jitter in beats
                            const auto r = gen(spec);
                            checkInvariants(r, 8.0 * style.beatsPerBar, label(spec));
                        }
}

TEST(drumpatterns, rock_backbeat)
{
    for (double intensity : { 0.0, 0.5, 1.0 })
        for (int variation : { 0, 1, 2 }) {
            DP::Spec spec;
            spec.style = "rock";
            spec.bars = 8;
            spec.intensity = intensity;
            spec.variation = variation;
            const auto r = gen(spec);
            for (int bar = 0; bar < 8; ++bar) {
                if (bar == 3 || bar == 7) continue;  // fill bars
                const double b = bar * 4.0;
                CHECK_MSG(hasAt(r.notes, DP::kKick, b), label(spec) + " kick, bar " + str(double(bar)));
                CHECK_MSG(hasAt(r.notes, DP::kSnare, b + 1), label(spec) + " snare 2, bar " + str(double(bar)));
                CHECK_MSG(hasAt(r.notes, DP::kSnare, b + 3), label(spec) + " snare 4, bar " + str(double(bar)));
            }
        }
}

TEST(drumpatterns, waltz_bars_are_three_beats)
{
    CHECK(beatsPerBar("waltz") == 3);
    for (int variation : { 0, 1 }) {
        DP::Spec spec;
        spec.style = "waltz";
        spec.bars = 6;
        spec.variation = variation;
        spec.fillEvery = 0;
        const auto r = gen(spec);
        checkInvariants(r, 18.0, label(spec));
        for (int bar = 0; bar < 6; ++bar) {
            CHECK_MSG(hasAt(r.notes, DP::kKick, bar * 3.0), "bar " + str(double(bar)));
            // Nothing lands on a 4/4 grid's beat 4 except later bars' downbeats.
            CHECK(!hasAt(r.notes, DP::kKick, bar * 3.0 + 1));
        }
        // Every bar repeats the same three beats.
        for (int bar = 2; bar < 6; ++bar)
            CHECK(countIn(r.notes, bar * 3.0, bar * 3.0 + 3, [](int) { return true; })
                  == countIn(r.notes, 3.0, 6.0, [](int) { return true; }));
        CHECK(r.notes.last().startBeat < 18.0);
    }
}

TEST(drumpatterns, reggae_one_drop)
{
    for (double intensity : { 0.0, 0.5, 1.0 })
        for (int variation : { 0, 1 }) {
            DP::Spec spec;
            spec.style = "reggae";
            spec.bars = 8;
            spec.intensity = intensity;
            spec.variation = variation;
            const auto r = gen(spec);
            for (int bar = 0; bar < 8; ++bar) {
                const double b = bar * 4.0;
                CHECK_MSG(!hasAt(r.notes, DP::kKick, b), label(spec) + " kick on 1, bar " + str(double(bar)));
                CHECK_MSG(hasAt(r.notes, DP::kKick, b + 2), label(spec) + " no kick on 3, bar " + str(double(bar)));
                CHECK_MSG(hasAt(r.notes, DP::kSideStick, b + 2) || hasAt(r.notes, DP::kSnare, b + 2),
                          label(spec) + " no rim on 3, bar " + str(double(bar)));
            }
        }
}

TEST(drumpatterns, swing_moves_offbeats_to_triplet)
{
    // Rock at medium intensity plays 8th hats; full swing puts each off-beat 8th
    // on the third triplet of the beat, 2/3 of the way through.
    DP::Spec spec;
    spec.style = "rock";
    spec.bars = 4;
    spec.fillEvery = 0;
    spec.swing = 100;
    auto r = gen(spec);
    int offbeats = 0;
    for (const auto& n : r.notes) {
        if (n.pitch != DP::kClosedHat) continue;
        const double f = frac(n.startBeat);
        if (f < kTol) continue;
        ++offbeats;
        CHECK_NEAR(f, 2.0 / 3.0, kTol);
    }
    CHECK(offbeats == 4 * 4);

    // Half swing goes half way: 1/2 + 1/12.
    spec.swing = 50;
    r = gen(spec);
    for (const auto& n : r.notes)
        if (n.pitch == DP::kClosedHat && frac(n.startBeat) > kTol)
            CHECK_NEAR(frac(n.startBeat), 0.5 + 1.0 / 12.0, kTol);

    // Straight rock leaves them on the 8th.
    spec.swing = -1;
    r = gen(spec);
    for (const auto& n : r.notes)
        if (n.pitch == DP::kClosedHat && frac(n.startBeat) > kTol)
            CHECK_NEAR(frac(n.startBeat), 0.5, kTol);

    // A 16th-swing style swings the 16ths instead: e and a move to the triplet
    // of each half beat, the 8th off-beat stays put.
    spec.style = "funk";
    spec.swing = 100;
    r = gen(spec);
    QSet<qint64> seen;
    for (const auto& n : r.notes) {
        if (n.pitch != DP::kClosedHat) continue;
        const double f = frac(n.startBeat);
        const double expected[] = { 0.0, 1.0 / 3.0, 0.5, 5.0 / 6.0 };
        bool ok = false;
        for (double e : expected) ok |= std::abs(f - e) <= kTol;
        CHECK_MSG(ok, "funk hat at " + str(f));
        seen.insert(std::llround(f * 6));
    }
    CHECK(seen.size() == 4);
}

TEST(drumpatterns, swing_defaults)
{
    QMap<QString, double> swing;
    for (const auto& s : DP::styles()) swing[s.name] = s.defaultSwing;
    CHECK(swing["blues_shuffle"] > 50);
    CHECK(swing["jazz_swing"] > 50);
    CHECK(swing["house"] == 0);
    CHECK(swing["rock"] == 0);

    // With no swing given, the shuffle's off-beats sit at its default swing.
    DP::Spec spec;
    spec.style = "blues_shuffle";
    spec.fillEvery = 0;
    const auto r = gen(spec);
    const double expected = 0.5 + swing["blues_shuffle"] / 100.0 / 6.0;
    int offbeats = 0;
    for (const auto& n : r.notes)
        if (n.pitch == DP::kClosedHat && frac(n.startBeat) > kTol) {
            ++offbeats;
            CHECK_NEAR(frac(n.startBeat), expected, kTol);
        }
    CHECK(offbeats > 0);

    // House stays straight: open hats exactly on the off-beat 8th.
    spec.style = "house";
    for (const auto& n : gen(spec).notes)
        if (n.pitch == DP::kOpenHat) CHECK_NEAR(frac(n.startBeat), 0.5, kTol);
}

TEST(drumpatterns, fills_and_crashes)
{
    for (const auto& style : DP::styles())
        for (double intensity : { 0.0, 0.5, 1.0 })
            for (int variation : { 0, 1 })
                for (int seed : { 1, 2, 3, 4 }) {
                    DP::Spec spec;
                    spec.style = style.name;
                    spec.bars = 8;
                    spec.intensity = intensity;
                    spec.variation = variation;
                    spec.seed = seed;
                    const auto r = gen(spec);
                    const int bpb = style.beatsPerBar;
                    // Bar 1 is a plain groove bar to compare the fill against.
                    const double plainStart = 1.0 * bpb + bpb - 1;
                    const int plainSnares = countIn(r.notes, plainStart, plainStart + 1, isSnareFamily);
                    for (int bar : { 3, 7 }) {
                        const double from = bar * bpb + bpb - 1.0, to = from + 1.0;
                        const int toms = countIn(r.notes, from, to, isTom);
                        const int snares = countIn(r.notes, from, to, isSnareFamily);
                        CHECK_MSG(toms > 0 || snares > plainSnares,
                                  label(spec) + " weak fill in bar " + str(double(bar)) + ": toms "
                                      + str(double(toms)) + " snares " + str(double(snares))
                                      + " vs " + str(double(plainSnares)));
                    }
                    // Crash on bar 1 and on the bar after the first fill, replacing the hat.
                    for (double at : { 0.0, 4.0 * bpb }) {
                        CHECK_MSG(hasAt(r.notes, DP::kCrash, at), label(spec) + " no crash at " + str(at));
                        for (int hat : { DP::kClosedHat, DP::kOpenHat })
                            CHECK_MSG(!hasAt(r.notes, hat, at), label(spec) + " hat under crash at " + str(at));
                    }
                    // No crash elsewhere on a downbeat.
                    for (int bar : { 1, 2, 3, 5, 6, 7 }) {
                        const double at = double(bar * bpb);
                        CHECK_MSG(!hasAt(r.notes, DP::kCrash, at), label(spec) + " stray crash at " + str(at));
                    }
                }
}

TEST(drumpatterns, fill_options)
{
    // Without fills or the opening crash every bar is the same groove.
    for (const auto& style : DP::styles()) {
        DP::Spec spec;
        spec.style = style.name;
        spec.bars = 6;
        spec.fillEvery = 0;
        spec.crashOnFirst = false;
        const auto r = gen(spec);
        const int bpb = style.beatsPerBar;
        QVector<QPair<int, double>> first;
        for (const auto& n : r.notes)
            if (n.startBeat < bpb) first.append({ n.pitch, n.startBeat });
        for (int bar = 1; bar < 6; ++bar) {
            QVector<QPair<int, double>> notes;
            for (const auto& n : r.notes)
                if (n.startBeat >= bar * bpb - kTol && n.startBeat < (bar + 1) * bpb - kTol)
                    notes.append({ n.pitch, n.startBeat - bar * bpb });
            bool same = notes.size() == first.size();
            for (int i = 0; same && i < notes.size(); ++i)
                same = notes[i].first == first[i].first && std::abs(notes[i].second - first[i].second) <= kTol;
            CHECK_MSG(same, str(style.name) + " bar " + str(double(bar)) + " differs");
        }
        CHECK_MSG(std::none_of(r.notes.begin(), r.notes.end(), [](const Note& n) { return isCrash(n.pitch); }),
                  style.name);
    }

    // A two-beat fill starts on beat 3 of the fill bar; beats 1-2 keep the groove.
    DP::Spec spec;
    spec.style = "rock";
    spec.bars = 4;
    spec.fillBeats = 2;
    spec.intensity = 0.5;
    const auto r = gen(spec);
    CHECK(hasAt(r.notes, DP::kSnare, 3 * 4.0 + 1));
    CHECK(!hasAt(r.notes, DP::kClosedHat, 3 * 4.0 + 2));  // beat 3 is the fill's
    CHECK(countIn(r.notes, 14.0, 16.0, isTom) + countIn(r.notes, 14.0, 16.0, isSnareFamily) >= 4);
}

TEST(drumpatterns, intensity_adds_notes)
{
    for (const auto& style : DP::styles())
        for (int variation : { 0, 1, 2 }) {
            int previous = -1, first = 0, last = 0;
            for (int step = 0; step <= 20; ++step) {
                DP::Spec spec;
                spec.style = style.name;
                spec.bars = 8;
                spec.variation = variation;
                spec.intensity = step / 20.0;
                const int count = gen(spec).notes.size();
                CHECK_MSG(count >= previous, label(spec) + ": " + str(double(count)) + " < " + str(double(previous)));
                previous = count;
                if (step == 0) first = count;
                last = count;
            }
            CHECK_MSG(last >= first * 3 / 2, str(style.name) + ": " + str(double(first)) + " -> " + str(double(last)));
        }

    // The busy end of rock: 16th hats and snare ghost notes.
    DP::Spec spec;
    spec.style = "rock";
    spec.intensity = 1.0;
    spec.fillEvery = 0;
    const auto r = gen(spec);
    CHECK(hasAt(r.notes, DP::kClosedHat, 1.25));
    CHECK(std::any_of(r.notes.begin(), r.notes.end(),
                      [](const Note& n) { return n.pitch == DP::kSnare && n.velocity < 60; }));
    spec.intensity = 0.0;
    const auto sparse = gen(spec);
    CHECK(!hasAt(sparse.notes, DP::kClosedHat, 1.5));  // quarter-note hats
}

TEST(drumpatterns, variation_and_determinism)
{
    for (const auto& style : DP::styles()) {
        DP::Spec spec;
        spec.style = style.name;
        spec.bars = 8;
        spec.humanize = 0.7;
        const auto a = gen(spec);
        CHECK_MSG(sameNotes(a.notes, gen(spec).notes), style.name);
        CHECK(!a.summary.isEmpty());

        DP::Spec other = spec;
        other.variation = 1;
        CHECK_MSG(!sameNotes(a.notes, gen(other).notes), str(style.name) + " variation 1 == 0");

        // A different seed re-rolls the humanize jitter.
        other = spec;
        other.seed = 2;
        CHECK_MSG(!sameNotes(a.notes, gen(other).notes), str(style.name) + " seed ignored");
    }
}

TEST(drumpatterns, humanize_bounds)
{
    for (double tempo : { 70.0, 174.0 })
        for (double humanize : { 0.5, 1.0 })
            for (const auto& style : DP::styles()) {
                DP::Spec spec;
                spec.style = style.name;
                spec.bars = 8;
                spec.intensity = 1.0;
                spec.tempo = tempo;
                const auto plain = gen(spec);
                spec.humanize = humanize;
                const auto human = gen(spec);
                CHECK_MSG(plain.notes.size() == human.notes.size(), label(spec));
                checkInvariants(human, 8.0 * style.beatsPerBar, label(spec));

                // Pair notes per drum in time order (humanize never reorders a drum).
                QMap<int, QVector<Note>> a, b;
                for (const auto& n : plain.notes) a[n.pitch].append(n);
                for (const auto& n : human.notes) b[n.pitch].append(n);
                const double boundMs = 10.0 * humanize;
                double largestMs = 0;
                int velocityChanges = 0;
                for (auto it = a.begin(); it != a.end(); ++it) {
                    const auto& p = it.value();
                    const auto& h = b.value(it.key());
                    if (p.size() != h.size()) { CHECK_MSG(false, label(spec)); continue; }
                    for (int i = 0; i < p.size(); ++i) {
                        const double ms = std::abs(h[i].startBeat - p[i].startBeat) * 60000.0 / tempo;
                        CHECK_MSG(ms <= boundMs + 1e-6, label(spec) + " moved " + str(ms) + " ms");
                        largestMs = std::max(largestMs, ms);
                        CHECK(std::abs(h[i].velocity - p[i].velocity) <= int(std::ceil(12 * humanize)) + 1);
                        velocityChanges += h[i].velocity != p[i].velocity;
                    }
                }
                // It does move things: with hundreds of notes some drift past half the bound.
                CHECK_MSG(largestMs > boundMs / 2, label(spec) + " largest drift " + str(largestMs));
                CHECK(velocityChanges > 0);
            }
}
