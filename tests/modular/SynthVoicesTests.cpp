#include "TestHarness.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// The "Synthesizers & Voices" family: drum voices and the FM voice.

using namespace OpenDaw::Tests;
using OpenDaw::Tests::ModuleRunner;

namespace {

constexpr double kC4 = 261.6255653005986;

// A gate that is high for `onSamples` from the start, then low.
std::vector<float> gate(int len, int onSamples)
{
    std::vector<float> g(size_t(len), 0.0f);
    std::fill(g.begin(), g.begin() + std::min(len, onSamples), 1.0f);
    return g;
}

// Frequency from interpolated upward zero crossings between from and to.
double measureHz(const std::vector<float>& s, int from, int to)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (s[size_t(i - 1)] < 0.0f && s[size_t(i)] >= 0.0f) {
            const double t = (i - 1) + s[size_t(i - 1)] / double(s[size_t(i - 1)] - s[size_t(i)]);
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
}

int at(double seconds) { return int(seconds * kRate); }

// Power at one frequency (Hann-windowed Goertzel over [from, to)), normalised
// so a full-scale sine filling the window reads 1.
double powerAt(const std::vector<float>& s, int from, int to, double hz)
{
    const int n = to - from;
    const double w = 2.0 * std::cos(6.283185307179586 * hz / kRate);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(6.283185307179586 * i / n);
        const double s0 = s[size_t(from + i)] * hann + w * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double mag = std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - w * s1 * s2));
    return std::pow(mag / (0.25 * n), 2.0);
}

// Share of the Hann-windowed energy in [from, to) that lies below `hz`, from
// the DFT bins (Parseval for the total).
double shareBelow(const std::vector<float>& s, int from, int to, double hz)
{
    const int n = to - from;
    std::vector<double> x(static_cast<size_t>(n));
    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        x[size_t(i)] = s[size_t(from + i)] * (0.5 - 0.5 * std::cos(6.283185307179586 * i / n));
        total += x[size_t(i)] * x[size_t(i)];
    }
    double below = 0.0;
    const int bins = int(hz * n / kRate);
    for (int k = 0; k <= bins; ++k) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i) {
            const double ph = 6.283185307179586 * double(k) * i / n;
            re += x[size_t(i)] * std::cos(ph);
            im -= x[size_t(i)] * std::sin(ph);
        }
        // Bins other than DC stand for themselves and their negative-frequency twin.
        below += (k == 0 ? 1.0 : 2.0) * (re * re + im * im) / n;
    }
    return total > 0.0 ? below / total : 0.0;
}

// RMS of consecutive `block`-sample blocks: a coarse amplitude envelope.
std::vector<double> blockRms(const std::vector<float>& s, int block)
{
    std::vector<double> env;
    for (int i = 0; i + block <= int(s.size()); i += block) env.push_back(rms(s, i, i + block));
    return env;
}

} // namespace

TEST(synthvoices, family)
{
    for (const char* type : { "kick", "snare", "hihat", "fm_voice", "clap", "tom", "cymbal",
                              "rimshot", "cowbell" }) {
        const auto* def = M::findModule(type);
        CHECK_MSG(def != nullptr, type);
        if (!def) continue;
        CHECK_MSG(def->category == M::Category::Voices, type);
        CHECK_MSG(def->allowedIn(M::Section::Voice) && def->allowedIn(M::Section::Global), type);
    }
}

TEST(synthvoices, kick_settles_to_its_tune_and_decays)
{
    ModuleRunner r("kick");
    r.setParam("tune", 50.0f);
    r.setParam("decay", 2.0f);
    r.setParam("click", 0.0f);
    r.setParam("drive", 0.0f);
    const int len = at(1.0);
    std::vector<float> trig(size_t(len), 0.0f);
    std::fill(trig.begin() + 100, trig.begin() + 200, 1.0f);
    const auto out = r.run(len, { { "trigger", trig } })[0];
    CHECK(allFinite(out));
    CHECK(rms(out, 0, 100) == 0.0);                  // silent until triggered
    CHECK(rms(out, 100, 100 + at(0.05)) > 0.4);
    // The pitch envelope (40 ms time constant, 3 octaves) is below 0.04% of an
    // octave by 0.35 s, so the measured pitch should be 50 Hz within the
    // zero-crossing measurement's ~0.05% over 12 cycles; allow 0.2%.
    const double hz = measureHz(out, at(0.35), at(0.6));
    CHECK_MSG(std::abs(hz / 50.0 - 1.0) < 0.002, "kick settles at " + str(hz) + " Hz");
    // It starts well above that (the sweep).
    CHECK(measureHz(out, 100, 100 + at(0.02)) > 100.0);

    // Decay is the time to -60 dB: by `decay` the envelope is at 0.001.
    ModuleRunner shortKick("kick");
    shortKick.setParam("decay", 0.3f);
    shortKick.setParam("click", 0.0f);
    shortKick.setParam("drive", 0.0f);
    const auto s = shortKick.run(at(0.5), { { "trigger", gate(at(0.5), 50) } })[0];
    const double ratio = rms(s, at(0.3), at(0.4)) / rms(s, 0, at(0.05));
    CHECK_MSG(ratio < 0.002, "level after the decay time: " + str(ratio));
}

TEST(synthvoices, accent_scales_each_hit)
{
    // The ride (with bell) runs its ring and bell paths, which the default crash doesn't.
    struct Case { const char* type; const char* param; float value; };
    for (const Case c : { Case{ "kick" }, Case{ "snare" }, Case{ "hihat" }, Case{ "clap" },
                          Case{ "tom" }, Case{ "cymbal" }, Case{ "cymbal", "kind", 1.0f },
                          Case{ "rimshot" }, Case{ "cowbell" } }) {
        const char* type = c.type;
        ModuleRunner full(type), soft(type);
        if (c.param) {
            full.setParam(c.param, c.value);
            soft.setParam(c.param, c.value);
            full.setParam("bell", 0.5f);
            soft.setParam("bell", 0.5f);
        }
        const auto trig = gate(4800, 100);
        const auto a = full.run(4800, { { "trigger", trig } })[0];
        const auto b = soft.run(4800, { { "trigger", trig },
                                        { "accent", std::vector<float>(4800, 0.25f) } })[0];
        CHECK_MSG(allFinite(a) && allFinite(b), type);
        // Same noise seed and DSP, so the soft hit is exactly a quarter of the full one.
        double worst = 0.0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(0.25 * a[i] - b[i]));
        CHECK_MSG(worst < 1e-6, std::string(type) + " accent error " + str(worst));
        CHECK_MSG(rms(a, 0, 4800) > 0.02, type);
    }
}

TEST(synthvoices, hihat_is_high_and_short)
{
    ModuleRunner r("hihat");
    r.setParam("decay", 0.08f);
    const auto out = r.run(at(0.3), { { "trigger", gate(at(0.3), 50) } })[0];
    CHECK(allFinite(out));
    // High-passed at 8 kHz: a 500 Hz one-pole lowpass of it keeps little energy.
    std::vector<float> low(out.size());
    const float c = float(std::exp(-6.283185307179586 * 500.0 / kRate));
    float y = 0.0f;
    for (size_t i = 0; i < out.size(); ++i) low[i] = y = (1.0f - c) * out[i] + c * y;
    const double lowRatio = rms(low, 0, at(0.05)) / rms(out, 0, at(0.05));
    CHECK_MSG(lowRatio < 0.1, "low-frequency share " + str(lowRatio));
    CHECK(rms(out, at(0.1), at(0.2)) < 0.002 * rms(out, 0, at(0.01)) + 1e-9);
}

TEST(synthvoices, snare_blends_tone_and_wires)
{
    // All tone: periodic at the head pitch. All wires: noise, no stable pitch.
    ModuleRunner tone("snare"), wires("snare");
    tone.setParam("snappy", 0.0f);
    tone.setParam("tone_decay", 1.0f);
    wires.setParam("snappy", 1.0f);
    const auto t = tone.run(at(0.5), { { "trigger", gate(at(0.5), 50) } })[0];
    const auto w = wires.run(at(0.5), { { "trigger", gate(at(0.5), 50) } })[0];
    CHECK(allFinite(t) && allFinite(w));
    CHECK(rms(t, 0, at(0.05)) > 0.1 && rms(w, 0, at(0.05)) > 0.05);
    // Zero crossings: the tone's are at most ~2 per cycle of its upper mode
    // (264.6 Hz); noise high-passed at 2 kHz crosses far more often.
    auto crossings = [](const std::vector<float>& s, int from, int to) {
        int n = 0;
        for (int i = from + 1; i < to; ++i) n += (s[size_t(i - 1)] < 0.0f) != (s[size_t(i)] < 0.0f);
        return n;
    };
    const int tc = crossings(t, at(0.1), at(0.2)), wc = crossings(w, 0, at(0.05)) * 2;
    CHECK_MSG(tc < 120 && wc > 300, "crossings tone " + str(tc) + ", wires " + str(wc));
}

TEST(synthvoices, fm_voice_tracks_pitch_and_releases)
{
    // Index 0: a pure sine carrier, so the pitch can be measured exactly.
    for (double octave : { 0.0, 1.0, -1.0 }) {
        ModuleRunner r("fm_voice");
        r.setParam("index", 0.0f);
        const int len = at(0.5);
        const auto out = r.run(len, { { "pitch", std::vector<float>(size_t(len), float(octave)) },
                                      { "gate", gate(len, len) } })[0];
        const double want = kC4 * std::pow(2.0, octave);
        const double hz = measureHz(out, at(0.1), at(0.5));
        // ~50-200 cycles measured: well under 0.01%; 0.15% (2.6 cents) is the bound.
        CHECK_MSG(std::abs(hz / want - 1.0) < 0.0015, "fm_voice at " + str(octave) + ": " + str(hz));
    }

    // Gate off: the release (0.3 s, ~95% done) takes it to near silence.
    ModuleRunner r("fm_voice");
    const int len = at(1.0);
    const auto out = r.run(len, { { "gate", gate(len, at(0.3)) } })[0];
    CHECK(allFinite(out));
    CHECK(rms(out, at(0.1), at(0.3)) > 0.2);
    CHECK(rms(out, at(0.8), at(1.0)) < 0.01 * rms(out, at(0.1), at(0.3)));

    // FM index changes the timbre, not the level envelope.
    ModuleRunner pure("fm_voice"), bright("fm_voice");
    pure.setParam("index", 0.0f);
    bright.setParam("index", 5.0f);
    const auto a = pure.run(at(0.2), { { "gate", gate(at(0.2), at(0.2)) } })[0];
    const auto b = bright.run(at(0.2), { { "gate", gate(at(0.2), at(0.2)) } })[0];
    double diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, double(std::abs(a[i] - b[i])));
    CHECK(diff > 0.3);
}

TEST(synthvoices, tom_settles_to_its_tune)
{
    // Floor and high tom, and the pitch input taking 150 Hz up an octave.
    struct Case { float tune, octave; double want; };
    for (const Case c : { Case{ 80.0f, 0.0f, 80.0 }, Case{ 300.0f, 0.0f, 300.0 },
                          Case{ 150.0f, 1.0f, 300.0 } }) {
        ModuleRunner r("tom");
        r.setParam("tune", c.tune);
        r.setParam("decay", 2.0f);
        r.setParam("attack", 0.0f);
        const int len = at(0.7);
        const auto out = r.run(len, { { "trigger", gate(len, 50) },
                                      { "pitch", std::vector<float>(size_t(len), c.octave) } })[0];
        CHECK(allFinite(out));
        // The 0.5-octave drop (30 ms time constant) is down to 2e-5 octave by
        // 0.3 s, and zero crossings of a decaying sine are exact, so the
        // measured pitch is within ~2e-5; 1e-4 (0.17 cent) is the bound.
        const double hz = measureHz(out, at(0.3), at(0.6));
        CHECK_MSG(std::abs(hz / c.want - 1.0) < 1e-4,
                  "tom at " + str(c.tune) + " + " + str(c.octave) + " oct: " + str(hz) + " Hz");
        // It starts above the tune: the first 20 ms average ~1/4 octave of sweep.
        CHECK(measureHz(out, 0, at(0.02)) > c.want * 1.1);
    }
}

TEST(synthvoices, clap_bursts_then_tail)
{
    for (float spread : { 0.01f, 0.015f }) {
        ModuleRunner r("clap");
        r.setParam("spread", spread);
        r.setParam("decay", 0.25f);
        const auto out = r.run(at(0.5), { { "trigger", gate(at(0.5), 50) } })[0];
        CHECK(allFinite(out));

        // Burst onsets: the 0.5 ms envelope rises above half its peak after
        // having fallen below 15% (each burst dies ~35 dB before the next).
        const int block = 24;
        const auto env = blockRms(out, block);
        const double peak = *std::max_element(env.begin(), env.end());
        std::vector<int> onsets;
        bool armed = true;
        const size_t burstWindow = size_t(at(4.5 * spread) / block);
        for (size_t b = 0; b < std::min(burstWindow, env.size()); ++b) {
            if (armed && env[b] > 0.5 * peak) { onsets.push_back(int(b) * block); armed = false; }
            if (env[b] < 0.15 * peak) armed = true;
        }
        CHECK_MSG(onsets.size() == 4, "clap bursts: " + str(double(onsets.size())));
        if (onsets.size() != 4) continue;
        // Onsets are quantised to a block and the threshold can fall a block
        // late on a quiet noise stretch: 2 blocks (1 ms) is the bound.
        for (size_t k = 1; k < onsets.size(); ++k) {
            const int gap = onsets[k] - onsets[k - 1];
            CHECK_MSG(std::abs(gap - at(spread)) <= 2 * block,
                      "burst gap " + str(gap / kRate * 1000.0) + " ms, spread " + str(spread * 1000.0));
        }

        // After the last burst a tail continues: no 5 ms stretch drops near
        // silence (it's still ~-11 dB of the start 40 ms in).
        const int last = onsets.back();
        const double first = rms(out, 0, at(0.005));
        double quietest = 1.0;
        for (int i = last; i + at(0.005) <= last + at(0.04); i += at(0.005))
            quietest = std::min(quietest, rms(out, i, i + at(0.005)) / first);
        CHECK_MSG(quietest > 0.1, "tail dips to " + str(quietest));
        // ...and falls 60 dB over `decay`. 40 ms windows of this ~800 Hz-wide
        // noise read within ~1.1 dB, so +/-3 dB (a ~5% decay error) is the bound.
        const int a = last + at(0.01), b = a + at(0.25);
        const double db = 20.0 * std::log10(rms(out, b, b + at(0.04)) / rms(out, a, a + at(0.04)));
        CHECK_MSG(std::abs(db + 60.0) < 3.0, "tail falls " + str(db) + " dB over its decay");
    }
}

TEST(synthvoices, cymbal_is_high_long_and_has_two_kinds)
{
    const float decay = 1.5f, color = 4000.0f;
    const double pingHz = color * 1.4;
    std::vector<float> crash, ride;
    for (int kind : { 0, 1 }) {
        ModuleRunner r("cymbal");
        r.setParam("decay", decay);
        r.setParam("color", color);
        r.setParam("kind", float(kind));
        (kind ? ride : crash) = r.run(at(2.0), { { "trigger", gate(at(2.0), 50) } })[0];
    }
    CHECK(allFinite(crash) && allFinite(ride));

    // A 4-pole high-pass at 4 kHz is >= 48 dB down at 1 kHz, so the source's
    // low energy (mostly the squares' 200-800 Hz fundamentals) leaves a share
    // around 1e-5 or less; window leakage adds less. 1e-3 (-30 dB) is the bound.
    for (const auto* s : { &crash, &ride }) {
        const double low = shareBelow(*s, 0, 4096, 1000.0);
        CHECK_MSG(low < 1e-3, "share below 1 kHz: " + str(low));
    }

    // Crash: one exponential wash, -60 dB at `decay`. The reference window is
    // centred at 10 ms (0.4 dB down already); +/-3 dB is a ~5% decay error.
    const double start = rms(crash, 0, at(0.02));
    CHECK(start > 0.1);
    const double atDecay = 20.0 * std::log10(rms(crash, at(0.95 * decay), at(1.05 * decay)) / start);
    CHECK_MSG(std::abs(atDecay + 59.6) < 3.0, "crash at its decay time: " + str(atDecay) + " dB");

    // Ride: its wash fades in 0.3 x decay, so by half the decay only the
    // ping ring is left, and that falls the remaining 30 dB by `decay`.
    const double ring = 20.0 * std::log10(rms(ride, at(0.95 * decay), at(1.05 * decay))
                                          / rms(ride, at(0.45 * decay), at(0.55 * decay)));
    CHECK_MSG(std::abs(ring + 30.0) < 3.0, "ride ring falls " + str(ring) + " dB over decay/2");
    CHECK(rms(ride, 0, at(0.02)) > 0.05);
    // The ride is shorter: once its wash is gone (0.3 x decay) only the ring,
    // starting ~10 dB under the hit, is left, so it has fallen well past the
    // crash's 18 dB. 6 dB further is the bound.
    auto fallBy = [&](const std::vector<float>& s) {
        return 20.0 * std::log10(rms(s, at(0.3 * decay), at(0.35 * decay)) / rms(s, 0, at(0.02)));
    };
    CHECK_MSG(fallBy(ride) < fallBy(crash) - 6.0,
              "fall by 0.3 x decay: ride " + str(fallBy(ride)) + " dB, crash " + str(fallBy(crash)));

    // Ride vs crash: late in the hit the ride's energy sits in the ping band,
    // the crash's spreads over the whole high-passed range.
    auto pingShare = [&](const std::vector<float>& s) {
        const int from = at(0.6), to = from + 4096;
        return shareBelow(s, from, to, pingHz * 1.25) - shareBelow(s, from, to, pingHz * 0.8);
    };
    const double rideShare = pingShare(ride), crashShare = pingShare(crash);
    CHECK_MSG(rideShare > 0.5 && crashShare < 0.35,
              "ping-band share ride " + str(rideShare) + ", crash " + str(crashShare));
}

TEST(synthvoices, rimshot_is_short_and_high)
{
    const float decay = 0.04f;
    ModuleRunner r("rimshot");
    r.setParam("decay", decay);
    r.setParam("tune", 1700.0f);
    const auto out = r.run(at(0.3), { { "trigger", gate(at(0.3), 50) } })[0];
    CHECK(allFinite(out));
    const double start = rms(out, 0, at(0.005));
    CHECK(start > 0.1);
    // Past twice its decay the envelope is at -120 dB (the click is gone
    // sooner); allow 20 dB for rounding.
    const double late = rms(out, at(2.0 * decay), at(0.3)) / start;
    CHECK_MSG(late < 1e-5, "after 2 x decay: " + str(20.0 * std::log10(late + 1e-30)) + " dB");
    // The tone is at `tune`: far stronger there than a few semitones either side.
    const double tone = powerAt(out, 0, at(decay), 1700.0);
    CHECK(tone > 1000.0 * std::max(powerAt(out, 0, at(decay), 1350.0),
                                   powerAt(out, 0, at(decay), 2150.0)));
}

TEST(synthvoices, cowbell_energy_is_at_its_two_tones)
{
    ModuleRunner r("cowbell");
    const auto out = r.run(at(0.3), { { "trigger", gate(at(0.3), 50) } })[0];
    CHECK(allFinite(out));
    const int to = at(0.1);
    const double low = powerAt(out, 0, to, 540.0), high = powerAt(out, 0, to, 800.0);
    // The band-pass sits at the tones' geometric mean, so both pass equally;
    // the 10 Hz analysis bins put each tone on a bin. 3 dB is the bound.
    CHECK_MSG(std::abs(10.0 * std::log10(high / low)) < 3.0,
              "540 Hz " + str(low) + ", 800 Hz " + str(high));
    // Two clean decaying lines leave ~-64 dB between and beside them (window
    // and decay leakage, 13+ bins away); a stray partial or a misplaced tone
    // would sit far above that. 40 dB below the weaker tone is the bound.
    for (double hz : { 670.0, 1100.0, 400.0 }) {
        const double off = powerAt(out, 0, to, hz);
        CHECK_MSG(off * 1.0e4 < std::min(low, high), "energy at " + str(hz) + " Hz: " + str(off));
    }
}

TEST(synthvoices, new_drums_stay_in_range_at_parameter_extremes)
{
    // Every corner of the parameter space, with fast retriggers and extreme
    // pitch, accent and garbage inputs.
    const int len = at(0.3);
    std::vector<float> trig(size_t(len), 0.0f);
    for (int i = 0; i < len; i += at(0.004)) trig[size_t(i)] = 1.0f;
    for (const char* type : { "clap", "tom", "cymbal", "rimshot", "cowbell" }) {
        const auto* def = M::findModule(type);
        CHECK_MSG(def != nullptr, type);
        if (!def) continue;
        const size_t n = def->params.size();
        for (unsigned corner = 0; corner < (1u << n); ++corner) {
            for (float pitch : { -10.0f, 10.0f }) {
                ModuleRunner r(type);
                for (size_t k = 0; k < n; ++k)
                    r.setParam(def->params[k].name.c_str(),
                               (corner >> k) & 1u ? def->params[k].max : def->params[k].min);
                std::vector<std::pair<const char*, std::vector<float>>> inputs = {
                    { "trigger", trig }, { "accent", std::vector<float>(size_t(len), 3.0f) } };
                if (def->inputIndex("pitch") >= 0)
                    inputs.push_back({ "pitch", std::vector<float>(size_t(len), pitch) });
                const auto out = r.run(len, inputs)[0];
                float worst = 0.0f;
                for (float v : out) worst = std::max(worst, std::abs(v));
                CHECK_MSG(allFinite(out) && worst <= 1.0f,
                          std::string(type) + " corner " + str(double(corner)) + " peak " + str(worst));
            }
        }
    }
}
