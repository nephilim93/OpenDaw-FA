#include "TestHarness.h"
#include <algorithm>
#include <cmath>

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

} // namespace

TEST(synthvoices, family)
{
    for (const char* type : { "kick", "snare", "hihat", "fm_voice" }) {
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
    for (const char* type : { "kick", "snare", "hihat" }) {
        ModuleRunner full(type), soft(type);
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
