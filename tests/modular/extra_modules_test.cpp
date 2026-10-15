#include "TestHarness.h"
#include <limits>

using namespace OpenDaw::Tests;
using OpenDaw::Modular::CompiledPatch;
using OpenDaw::Modular::Transport;

namespace {

std::unique_ptr<CompiledPatch> compile(const QJsonObject& json)
{
    auto cp = CompiledPatch::build(patchFrom(json), kRate);
    CHECK(cp != nullptr);
    return cp;
}

// Renders a patch with no notes playing (global-section modules only).
Stereo renderGlobal(const QJsonArray& modules, const QJsonArray& cables, int numSamples)
{
    auto cp = compile(patchJson(modules, cables));
    if (!cp) return {};
    return renderPatch(*cp, numSamples);
}

QJsonObject output() { return mod("out", "output", { { "volume", 1 } }); }

// A global sine at C4 + `semitones`, full level.
QJsonObject sine(const char* id, double semitones)
{
    return mod(id, "vco", { { "wave", "sine" }, { "tune", semitones }, { "level", 1 } }, "global");
}

double hzOf(double semitones) { return 261.6255653005986 * std::pow(2.0, semitones / 12.0); }

// Amplitude of the component at `hz` (Hann-windowed DFT bin, so nearby tones
// a few tens of Hz away do not leak in).
double amplitudeAt(const std::vector<float>& s, int from, int to, double hz)
{
    const double w = 2.0 * 3.141592653589793 * hz / kRate;
    double re = 0.0, im = 0.0, wsum = 0.0;
    const int n = to - from;
    for (int i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / (n - 1));
        const double v = s[size_t(from + i)] * win;
        re += v * std::cos(w * i);
        im -= v * std::sin(w * i);
        wsum += win;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// Fundamental from normalised autocorrelation: the shortest lag whose peak is
// within 10% of the best one (so octave-down lags don't win), refined by a
// parabola through its neighbours.
double autocorrHz(const std::vector<float>& s, int from, int len, double minHz, double maxHz)
{
    const int minLag = int(kRate / maxHz), maxLag = int(kRate / minHz) + 1;
    std::vector<double> r(size_t(maxLag + 2), 0.0);
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) {
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (int i = from; i < from + len; ++i) {
            const double x = s[size_t(i)], y = s[size_t(i + lag)];
            xy += x * y; xx += x * x; yy += y * y;
        }
        r[size_t(lag)] = xx > 0.0 && yy > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
    }
    double best = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, r[size_t(lag)]);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double a = r[size_t(lag - 1)], b = r[size_t(lag)], c = r[size_t(lag + 1)];
        if (b >= 0.9 * best && b >= a && b >= c) {
            const double denom = a - 2.0 * b + c;
            const double shift = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
            return kRate / (lag + shift);
        }
    }
    return 0.0;
}

std::vector<float> sineSignal(int len, double hz, float amp)
{
    std::vector<float> v(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) v[size_t(i)] = amp * float(std::sin(2.0 * 3.141592653589793 * hz * i / kRate));
    return v;
}

// A gate train: high for the first half of every `period` samples.
std::vector<float> pulses(int len, int period)
{
    std::vector<float> v(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) v[size_t(i)] = (i % period) < period / 2 ? 1.0f : 0.0f;
    return v;
}

// The euclidean gate sampled just after each clock edge, as "x" / ".".
std::string euclidPattern(int steps, int pulsesPerBar, int rotation, int numSteps)
{
    constexpr int kStep = 960;  // free clock at 50 Hz
    const auto out = renderGlobal(
        { mod("clk", "clock", { { "mode", "free" }, { "rate", 50 } }, "global"),
          mod("euc", "euclidean", { { "steps", steps }, { "pulses", pulsesPerBar }, { "rotation", rotation } },
              "global"),
          output() },
        { cable("clk.gate", "euc.clock"), cable("euc.gate", "out.left"), cable("clk.gate", "out.right") },
        numSteps * kStep);
    std::string pattern;
    for (int edge : risingEdges(out.right))
        pattern += out.left[size_t(edge + 1)] > 0.5f ? 'x' : '.';
    return pattern;
}

} // namespace

TEST(extra, ring_mod_gives_sum_and_difference)
{
    // 440 Hz * 261.63 Hz -> 701.63 Hz and 178.37 Hz at half amplitude each.
    const double fa = hzOf(9.0), fb = hzOf(0.0);
    const int len = int(kRate);
    const auto out = renderGlobal(
        { sine("a", 9.0), sine("b", 0.0), mod("ring", "ring_mod", {}, "global"), output() },
        { cable("a.out", "ring.a"), cable("b.out", "ring.b"), cable("ring.out", "out.left") }, len);
    const auto& s = out.left;
    CHECK(allFinite(s));
    const double sum = amplitudeAt(s, 0, len, fa + fb), diff = amplitudeAt(s, 0, len, fa - fb);
    const double carrierA = amplitudeAt(s, 0, len, fa), carrierB = amplitudeAt(s, 0, len, fb);
    CHECK_NEAR(sum, 0.5, 0.01);
    CHECK_NEAR(diff, 0.5, 0.01);
    CHECK_MSG(carrierA < 0.005 && carrierB < 0.005, str(carrierA) + " " + str(carrierB));

    // mix 0 passes a through untouched.
    const auto dry = renderGlobal(
        { sine("a", 9.0), sine("b", 0.0), mod("ring", "ring_mod", { { "mix", 0 } }, "global"), output() },
        { cable("a.out", "ring.a"), cable("b.out", "ring.b"), cable("ring.out", "out.left") }, len);
    CHECK_NEAR(amplitudeAt(dry.left, 0, len, fa), 1.0, 0.01);
    CHECK(amplitudeAt(dry.left, 0, len, fa + fb) < 0.005);
}

// About 5 cents. The estimator interpolates between lags, so this is well
// above its error; 1% let a half-sample tuning error at 880 Hz pass.
constexpr double kCombPitchTolerance = 0.003;

TEST(extra, comb_filter_plucks_at_pitch)
{
    // Karplus-Strong: a 5 ms noise burst into the comb, tuned by the voice pitch.
    auto pluck = [](int note, double tune, double feedback) {
        auto cp = compile(patchJson(
            { mod("vin", "voice_in"), mod("noise", "noise", { { "level", 1 } }),
              mod("env", "adsr", { { "attack", 0 }, { "decay", 0.005 }, { "sustain", 0 }, { "release", 0.005 } }),
              mod("amp", "vca"),
              mod("comb", "comb_filter", { { "tune", tune }, { "feedback", feedback }, { "damping", 0.5 } }),
              mod("vout", "voice_out"), mod("voices", "voices"), output() },
            { cable("noise.out", "amp.in"), cable("vin.gate", "env.gate"), cable("env.out", "amp.cv"),
              cable("amp.out", "comb.in"), cable("vin.pitch", "comb.pitch"), cable("comb.out", "vout.left"),
              cable("voices.left", "out.left") }));
        if (!cp) return Stereo{};
        cp->noteOn(note, 1.0f);
        return renderPatch(*cp, int(kRate * 0.5));
    };

    for (int note : { 45, 57, 64, 69, 81 }) {
        const double expected = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const auto out = pluck(note, 0.0, 0.95);
        CHECK(allFinite(out.left));
        const double hz = autocorrHz(out.left, int(kRate * 0.05), 4800, 50.0, 2000.0);
        CHECK_MSG(std::abs(hz / expected - 1.0) < kCombPitchTolerance,
                  "note " + str(double(note)) + ": " + str(hz) + " Hz, expected " + str(expected));
        // It rings, then decays.
        const double early = rms(out.left, int(kRate * 0.02), int(kRate * 0.07));
        const double late = rms(out.left, int(kRate * 0.45), int(kRate * 0.5));
        CHECK_MSG(early > 0.01 && late < early * 0.5, str(early) + " " + str(late));
    }

    // tune shifts the pitch: A3 + 12 semitones plucks at A4.
    const double tuned = autocorrHz(pluck(57, 12.0, 0.95).left, int(kRate * 0.05), 4800, 50.0, 2000.0);
    CHECK_MSG(std::abs(tuned / 440.0 - 1.0) < kCombPitchTolerance, str(tuned));

    // Negative feedback resonates an octave lower.
    const double negative = autocorrHz(pluck(69, 0.0, -0.95).left, int(kRate * 0.05), 4800, 50.0, 2000.0);
    CHECK_MSG(std::abs(negative / 220.0 - 1.0) < kCombPitchTolerance, str(negative));
}

TEST(extra, wavefolder_folds)
{
    const double f = hzOf(9.0);
    const int len = int(kRate);
    auto fold = [&](const QJsonObject& params) {
        return renderGlobal(
            { sine("osc", 9.0), mod("wf", "wavefolder", params, "global"), output() },
            { cable("osc.out", "wf.in"), cable("wf.out", "out.left") }, len).left;
    };
    // drive 1: a -1..1 sine is inside the fold range and comes out clean.
    const auto clean = fold({ { "drive", 1 } });
    CHECK_NEAR(amplitudeAt(clean, 0, len, f), 1.0, 0.01);
    CHECK(amplitudeAt(clean, 0, len, 3.0 * f) < 0.005);

    // drive 3: folded back into -1..1 with strong odd harmonics, no even ones.
    const auto folded = fold({ { "drive", 3 } });
    CHECK(allFinite(folded));
    // (The DC blocker may ripple a percent past the fold edges.)
    CHECK(*std::max_element(folded.begin(), folded.end()) <= 1.02f);
    CHECK(*std::min_element(folded.begin(), folded.end()) >= -1.02f);
    CHECK_MSG(amplitudeAt(folded, 0, len, 3.0 * f) > 0.1, str(amplitudeAt(folded, 0, len, 3.0 * f)));
    CHECK(amplitudeAt(folded, 0, len, 2.0 * f) < 0.005);

    // symmetry adds even harmonics, and the DC it leaves is blocked.
    const auto asym = fold({ { "drive", 3 }, { "symmetry", 0.5 } });
    CHECK_MSG(amplitudeAt(asym, 0, len, 2.0 * f) > 0.05, str(amplitudeAt(asym, 0, len, 2.0 * f)));
    double mean = 0.0;
    for (int i = len / 2; i < len; ++i) mean += asym[size_t(i)];
    mean /= len / 2;
    CHECK_NEAR(mean, 0.0, 0.01);

    // The fold input adds 4 units of drive per unit: drive 1 + fold 0.5 == drive 3.
    OpenDaw::Tests::ModuleRunner a("wavefolder"), b("wavefolder");
    a.setParam("drive", 1.0f);
    b.setParam("drive", 3.0f);
    const auto in = sineSignal(4800, 440.0, 0.9f);
    const auto viaCv = a.run(4800, { { "in", in }, { "fold", std::vector<float>(4800, 0.5f) } })[0];
    const auto viaParam = b.run(4800, { { "in", in } })[0];
    double maxDiff = 0.0;
    for (size_t i = 0; i < viaCv.size(); ++i) maxDiff = std::max(maxDiff, double(std::abs(viaCv[i] - viaParam[i])));
    CHECK_NEAR(maxDiff, 0.0, 1e-5);
}

TEST(extra, chorus_is_stereo_finite_and_bounded)
{
    const int len = int(kRate * 2);
    auto chorus = [&](const QJsonObject& params) {
        return renderGlobal(
            { mod("osc", "vco", { { "wave", "saw" }, { "level", 1 } }, "global"),
              mod("ch", "chorus", params, "global"), output() },
            { cable("osc.out", "ch.left"), cable("ch.left", "out.left"), cable("ch.right", "out.right") }, len);
    };
    for (const char* voices : { "2", "3", "4" }) {
        const auto out = chorus({ { "voices", voices }, { "depth", 1 }, { "mix", 0.5 }, { "rate", 3 } });
        for (const auto* ch : { &out.left, &out.right }) {
            CHECK(allFinite(*ch));
            CHECK(*std::max_element(ch->begin(), ch->end()) <= 1.2f);
            CHECK(*std::min_element(ch->begin(), ch->end()) >= -1.2f);
            CHECK_MSG(rms(*ch, len / 2, len) > 0.2, std::string(voices) + " " + str(rms(*ch, len / 2, len)));
        }
        // A mono input comes out different on each side.
        double diff = 0.0;
        for (int i = len / 2; i < len; ++i) diff += std::abs(out.left[size_t(i)] - out.right[size_t(i)]);
        CHECK_MSG(diff / (len / 2) > 0.01, std::string(voices) + " " + str(diff / (len / 2)));
    }

    // mix 0 is the dry signal.
    const auto dry = chorus({ { "mix", 0 } });
    const auto direct = renderGlobal(
        { mod("osc", "vco", { { "wave", "saw" }, { "level", 1 } }, "global"), output() },
        { cable("osc.out", "out.left") }, len);
    CHECK(dry.left == direct.left);

    // NaN and huge inputs: finite, never louder than the input.
    OpenDaw::Tests::ModuleRunner r("chorus");
    r.setParam("mix", 1.0f);
    r.setParam("voices", 2.0f);
    std::vector<float> wild(static_cast<size_t>(kRate));
    for (size_t i = 0; i < wild.size(); ++i)
        wild[i] = i % 997 == 0 ? std::numeric_limits<float>::quiet_NaN() : ((i / 40) % 2 ? 4.0f : -4.0f);
    for (const auto& ch : r.run(int(wild.size()), { { "left", wild } })) {
        CHECK(allFinite(ch));
        CHECK(*std::max_element(ch.begin(), ch.end()) <= 4.0f);
        CHECK(*std::min_element(ch.begin(), ch.end()) >= -4.0f);
    }
}

TEST(extra, envelope_follower_tracks_level)
{
    const int len = int(kRate * 0.5);
    const auto out = renderGlobal(
        { mod("osc", "vco", { { "wave", "sine" }, { "tune", 9 }, { "level", 0.5 } }, "global"),
          mod("ef", "envelope_follower", { { "attack", 0.001 }, { "release", 0.2 } }, "global"), output() },
        { cable("osc.out", "ef.in"), cable("ef.out", "out.left") }, len);
    double lo = 1.0, hi = 0.0;
    for (int i = len / 2; i < len; ++i) {
        lo = std::min(lo, double(out.left[size_t(i)]));
        hi = std::max(hi, double(out.left[size_t(i)]));
    }
    CHECK_MSG(lo > 0.45 && hi <= 0.5, str(lo) + " " + str(hi));

    // When the input stops it falls to 1/e after 'release' seconds.
    OpenDaw::Tests::ModuleRunner r("envelope_follower");
    r.setParam("attack", 0.001f);
    r.setParam("release", 0.2f);
    auto in = sineSignal(int(kRate), 440.0, 0.5f);
    std::fill(in.begin() + len, in.end(), 0.0f);
    const auto env = r.run(int(kRate), { { "in", in } })[0];
    const double atStop = env[size_t(len)];
    CHECK_NEAR(env[size_t(len + int(kRate * 0.2))] / atStop, std::exp(-1.0), 0.05);

    // Clamped to 0..1 for loud input.
    const auto loud = r.run(4800, { { "in", std::vector<float>(4800, 5.0f) } })[0];
    CHECK(*std::max_element(loud.begin(), loud.end()) <= 1.0f);
}

TEST(extra, comparator_with_hysteresis)
{
    // A 10 Hz lfo against 0: a 50% square gate, ten edges a second.
    const int len = int(kRate);
    const auto out = renderGlobal(
        { mod("lfo", "lfo", { { "rate", 10 } }, "global"), mod("cmp", "comparator", {}, "global"), output() },
        { cable("lfo.out", "cmp.a"), cable("cmp.gate", "out.left") }, len);
    CHECK(risingEdges(out.left).size() == 10);
    int high = 0;
    for (float v : out.left) high += v > 0.5f;
    CHECK_NEAR(high, len / 2, len * 0.01);

    // A noisy slow sine: no hysteresis chatters, enough hysteresis switches once per cycle.
    std::vector<float> noisy = sineSignal(len, 5.0, 1.0f);
    uint32_t seed = 12345;
    for (auto& v : noisy) {
        seed = seed * 1664525u + 1013904223u;
        v += (float(seed >> 8) / 16777216.0f - 0.5f) * 0.1f;  // +/-0.05
    }
    OpenDaw::Tests::ModuleRunner bare("comparator"), schmitt("comparator");
    bare.setParam("hysteresis", 0.0f);
    schmitt.setParam("hysteresis", 0.2f);
    const auto chatter = risingEdges(bare.run(len, { { "a", noisy } })[0]).size();
    const auto clean = risingEdges(schmitt.run(len, { { "a", noisy } })[0]).size();
    CHECK_MSG(chatter > 5, str(double(chatter)));
    CHECK_MSG(clean == 5, str(double(clean)));

    // b sets the threshold.
    const auto above = schmitt.run(160, { { "a", std::vector<float>(160, 0.3f) },
                                          { "b", std::vector<float>(160, 0.5f) } })[0];
    CHECK(above.back() == 0.0f);
}

TEST(extra, logic_truth_tables)
{
    // a and b step through 00, 01, 10, 11 (32 samples each).
    std::vector<float> a(128), b(128);
    for (int i = 0; i < 128; ++i) {
        a[size_t(i)] = (i / 32) >= 2 ? 1.0f : 0.0f;
        b[size_t(i)] = (i / 32) % 2 ? 1.0f : 0.0f;
    }
    const std::pair<const char*, std::array<float, 4>> tables[] = {
        { "and", { 0, 0, 0, 1 } }, { "or", { 0, 1, 1, 1 } },
        { "xor", { 0, 1, 1, 0 } }, { "not", { 1, 1, 0, 0 } },
    };
    for (int op = 0; op < 4; ++op) {
        OpenDaw::Tests::ModuleRunner r("logic");
        r.setParam("op", float(op));
        const auto out = r.run(128, { { "a", a }, { "b", b } })[0];
        for (int row = 0; row < 4; ++row)
            CHECK_MSG(out[size_t(row * 32 + 16)] == tables[op].second[size_t(row)],
                      std::string(tables[op].first) + " row " + str(double(row)));
    }

    // In a patch: a clock anded with its own /4 square keeps the first two of every four pulses.
    const auto out = renderGlobal(
        { mod("fast", "clock", { { "mode", "free" }, { "rate", 50 } }, "global"),
          mod("div", "clock_divider", {}, "global"),
          mod("lg", "logic", { { "op", "and" } }, "global"), output() },
        { cable("fast.gate", "div.clock"), cable("fast.gate", "lg.a"), cable("div.div_4", "lg.b"),
          cable("lg.out", "out.left") }, 9600);
    const auto edges = risingEdges(out.left);
    CHECK_MSG((edges == std::vector<int>{ 0, 960, 3840, 4800, 7680, 8640 }), str(double(edges.size())));
}

TEST(extra, crossfade_blends)
{
    const double fa = hzOf(0.0), fb = hzOf(7.0);
    const int len = int(kRate * 0.5);
    for (double m : { 0.0, 0.25, 1.0 }) {
        const auto out = renderGlobal(
            { sine("a", 0.0), sine("b", 7.0), mod("x", "crossfade", { { "mix", m } }, "global"), output() },
            { cable("a.out", "x.a"), cable("b.out", "x.b"), cable("x.out", "out.left") }, len);
        CHECK_NEAR(amplitudeAt(out.left, 0, len, fa), 1.0 - m, 0.01);
        CHECK_NEAR(amplitudeAt(out.left, 0, len, fb), m, 0.01);
    }
    // The mix input adds to the param and the sum is clamped to 0..1.
    OpenDaw::Tests::ModuleRunner r("crossfade");
    r.setParam("mix", 0.2f);
    const std::vector<float> a(64, 1.0f), b(64, 3.0f);
    CHECK_NEAR(r.run(64, { { "a", a }, { "b", b }, { "mix", std::vector<float>(64, 0.5f) } })[0][10], 2.4, 1e-5);
    CHECK_NEAR(r.run(64, { { "a", a }, { "b", b }, { "mix", std::vector<float>(64, 2.0f) } })[0][10], 3.0, 1e-6);
    CHECK_NEAR(r.run(64, { { "a", a }, { "b", b } })[0][10], 1.4, 1e-5);
}

TEST(extra, clock_divider_counts)
{
    constexpr int kStep = 960;  // free clock at 50 Hz
    const int len = 48 * kStep;
    const auto out = renderGlobal(
        { mod("clk", "clock", { { "mode", "free" }, { "rate", 50 } }, "global"),
          mod("div", "clock_divider", {}, "global"), output() },
        { cable("clk.gate", "div.clock"), cable("div.div_3", "out.left"), cable("div.div_8", "out.right") },
        len);
    const auto e3 = risingEdges(out.left), e8 = risingEdges(out.right);
    CHECK_MSG(e3.size() == 16 && e8.size() == 6, str(double(e3.size())) + " " + str(double(e8.size())));
    for (size_t k = 0; k < e3.size(); ++k) CHECK(e3[k] == int(k) * 3 * kStep);
    for (size_t k = 0; k < e8.size(); ++k) CHECK(e8[k] == int(k) * 8 * kStep);
    // Square waves: half of each period high, even for /3.
    int high3 = 0, high8 = 0;
    for (int i = 0; i < len; ++i) { high3 += out.left[size_t(i)] > 0.5f; high8 += out.right[size_t(i)] > 0.5f; }
    CHECK_NEAR(high3, len / 2, kStep * 0.05);
    CHECK_NEAR(high8, len / 2, kStep * 0.05);

    // /2 and /4 directly.
    const auto clock = pulses(10 * 100, 100);
    const auto outs = OpenDaw::Tests::ModuleRunner("clock_divider").run(int(clock.size()), { { "clock", clock } });
    CHECK((risingEdges(outs[0]) == std::vector<int>{ 0, 200, 400, 600, 800 }));
    CHECK((risingEdges(outs[2]) == std::vector<int>{ 0, 400, 800 }));

    // Reset after clock 3 (at 200): clock 4 (at 300) counts as the first again.
    std::vector<float> reset(clock.size(), 0.0f);
    reset[250] = 1.0f;
    const auto afterReset = OpenDaw::Tests::ModuleRunner("clock_divider")
                                .run(int(clock.size()), { { "clock", clock }, { "reset", reset } });
    CHECK((risingEdges(afterReset[1]) == std::vector<int>{ 0, 300, 600, 900 }));
}

TEST(extra, euclidean_patterns)
{
    CHECK_MSG(euclidPattern(16, 5, 0, 32) == "x..x..x..x..x...x..x..x..x..x...", euclidPattern(16, 5, 0, 32));
    CHECK_MSG(euclidPattern(8, 3, 0, 8) == "x..x..x.", euclidPattern(8, 3, 0, 8));
    CHECK_MSG(euclidPattern(8, 5, 0, 8) == "x.xx.xx.", euclidPattern(8, 5, 0, 8));
    CHECK_MSG(euclidPattern(13, 5, 0, 13) == "x..x.x..x.x..", euclidPattern(13, 5, 0, 13));
    CHECK_MSG(euclidPattern(4, 4, 0, 4) == "xxxx", euclidPattern(4, 4, 0, 4));
    CHECK_MSG(euclidPattern(4, 0, 0, 4) == "....", euclidPattern(4, 0, 0, 4));
    // pulses beyond steps clamp to every step.
    CHECK_MSG(euclidPattern(3, 9, 0, 3) == "xxx", euclidPattern(3, 9, 0, 3));
    // Rotation 2: step 1 plays pattern step 3.
    CHECK_MSG(euclidPattern(16, 5, 2, 16) == ".x..x..x..x...x.", euclidPattern(16, 5, 2, 16));

    // Reset: the next clock goes back to step 1.
    OpenDaw::Tests::ModuleRunner r("euclidean");
    r.setParam("steps", 8.0f);
    r.setParam("pulses", 3.0f);
    const auto clock = pulses(12 * 100, 100);
    std::vector<float> reset(clock.size(), 0.0f);
    reset[450] = 1.0f;  // after step 5 -> clock at 500 is step 1 (a hit)
    const auto gate = r.run(int(clock.size()), { { "clock", clock }, { "reset", reset } })[0];
    std::string got;
    for (int k = 0; k < 12; ++k) got += gate[size_t(k * 100 + 1)] > 0.5f ? 'x' : '.';
    CHECK_MSG(got == "x..x.x..x..x", got);
}

TEST(extra, survive_bad_input)
{
    // NaN, infinity and huge values on every input: outputs stay finite.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> wild(4800);
    for (size_t i = 0; i < wild.size(); ++i)
        wild[i] = i % 3 == 0 ? nan : i % 3 == 1 ? inf : (i % 2 ? 1e30f : -1e30f);
    for (const char* type : { "wavefolder", "ring_mod", "comb_filter", "chorus", "envelope_follower",
                              "comparator", "logic", "crossfade", "clock_divider", "euclidean" }) {
        OpenDaw::Tests::ModuleRunner r(type);
        const auto* def = OpenDaw::Modular::findModule(type);
        if (!def) continue;
        std::vector<std::pair<const char*, std::vector<float>>> inputs;
        for (const auto& port : def->inputs) inputs.push_back({ port.name.c_str(), wild });
        for (const auto& out : r.run(int(wild.size()), inputs))
            CHECK_MSG(allFinite(out), type);
        // And recover once the input is sane again.
        inputs.clear();
        for (const auto& port : def->inputs) inputs.push_back({ port.name.c_str(), sineSignal(4800, 220.0, 0.5f) });
        for (const auto& out : r.run(4800, inputs))
            CHECK_MSG(allFinite(out), type);
    }
}
