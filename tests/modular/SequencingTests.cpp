#include "TestHarness.h"

using namespace OpenDaw::Tests;
using OpenDaw::Modular::CompiledPatch;
using OpenDaw::Modular::Transport;

namespace {

// 120 bpm at 48 kHz: one beat is 24000 samples, a 1/16 step 6000.
constexpr int kBeat = 24000;
constexpr int kSixteenth = kBeat / 4;

Transport playing(double beat = 0.0)
{
    Transport t;
    t.bpm = 120.0;
    t.beat = beat;
    t.playing = true;
    return t;
}

// Renders `source` (a global module output) to the left channel unscaled.
Stereo renderGlobal(const QJsonObject& module, const char* left, const char* right, int numSamples,
                    const Transport& transport)
{
    auto cp = CompiledPatch::build(
        patchFrom(patchJson({ module, mod("out", "output", { { "volume", 1 } }) },
                            { cable(left, "out.left"), cable(right, "out.right") })),
        kRate);
    CHECK(cp != nullptr);
    if (!cp) return {};
    return renderPatch(*cp, numSamples, transport);
}

std::string edgesText(const std::vector<int>& edges)
{
    std::string s;
    for (int e : edges) s += std::to_string(e) + " ";
    return s;
}

} // namespace

TEST(sequencing, clock_sixteenths_on_the_grid)
{
    const auto out = renderGlobal(mod("clk", "clock", { { "division", "1/16" } }, "global"),
                                  "clk.trigger", "clk.gate", kBeat * 2, playing());
    const auto triggers = risingEdges(out.left);
    const auto gates = risingEdges(out.right);
    CHECK_MSG(triggers.size() == 8, edgesText(triggers));
    for (size_t k = 0; k < triggers.size(); ++k)
        CHECK_MSG(triggers[k] == int(k) * kSixteenth, edgesText(triggers));
    CHECK(gates == triggers);

    // The gate is high for half of each step by default.
    int high = 0;
    for (int i = 0; i < kSixteenth; ++i) high += out.right[size_t(i)] > 0.5f;
    CHECK_NEAR(high, kSixteenth / 2, 2);

    // Starting mid-song still lands on the grid: from beat 2.1 the next step
    // starts at beat 2.25.
    const auto late = renderGlobal(mod("clk", "clock", { { "division", "1/16" } }, "global"),
                                   "clk.trigger", "clk.gate", kBeat, playing(2.1));
    const auto lateEdges = risingEdges(late.left);
    CHECK_MSG(!lateEdges.empty() && std::abs(lateEdges[0] - int(0.15 * kBeat)) <= 1, edgesText(lateEdges));

    // Stopped transport: no output in sync mode.
    Transport stopped = playing();
    stopped.playing = false;
    const auto silent = renderGlobal(mod("clk", "clock", {}, "global"), "clk.trigger", "clk.gate",
                                     kBeat, stopped);
    CHECK(risingEdges(silent.left).empty() && risingEdges(silent.right).empty());
}

TEST(sequencing, clock_swing_delays_odd_steps)
{
    const auto out = renderGlobal(
        mod("clk", "clock", { { "division", "1/16" }, { "swing", 0.5 } }, "global"),
        "clk.trigger", "clk.gate", kBeat * 2, playing());
    const auto edges = risingEdges(out.left);
    CHECK_MSG(edges.size() == 8, edgesText(edges));
    // Even steps stay on the grid, odd steps move by swing * half a step.
    for (size_t k = 0; k < edges.size(); ++k) {
        const int expected = int(k) * kSixteenth + (k % 2 ? kSixteenth / 4 : 0);
        CHECK_MSG(edges[k] == expected, edgesText(edges));
    }
}

TEST(sequencing, step_sequencer_pitches_and_gates)
{
    // Four steps: C4, Eb4, G4 (rest), C5; clocked by 1/16 gates.
    const QJsonObject seq = mod("seq", "step_sequencer",
        { { "steps", 4 }, { "pitch_1", 0 }, { "pitch_2", 3 }, { "pitch_3", 7 }, { "pitch_4", 12 },
          { "gate_3", "off" } }, "global");
    auto cp = CompiledPatch::build(patchFrom(patchJson(
        { mod("clk", "clock", { { "division", "1/16" } }, "global"), seq,
          mod("out", "output", { { "volume", 1 } }) },
        { cable("clk.gate", "seq.clock"), cable("seq.pitch", "out.left"),
          cable("seq.gate", "out.right") })), kRate);
    CHECK(cp != nullptr);
    if (!cp) return;
    const auto out = renderPatch(*cp, kSixteenth * 8, playing());

    const float pitches[] = { 0.0f, 3.0f, 7.0f, 12.0f };
    const bool gated[] = { true, true, false, true };
    for (int step = 0; step < 8; ++step) {
        const int k = step % 4;
        const size_t mid = size_t(step * kSixteenth + kSixteenth / 4);  // inside the gate
        const size_t rest = size_t(step * kSixteenth + kSixteenth * 3 / 4);  // after it
        CHECK_MSG(std::abs(out.left[mid] - pitches[k] / 12.0f) < 1e-5f,
                  "step " + str(double(step)) + " pitch " + str(out.left[mid] * 12.0));
        CHECK_MSG((out.right[mid] > 0.5f) == gated[k], "step " + str(double(step)) + " gate");
        CHECK(out.right[rest] == 0.0f);
    }
    // Gates start exactly on the step.
    const auto edges = risingEdges(out.right);
    CHECK_MSG(edges.size() == 6 && edges[0] == 0 && edges[1] == kSixteenth
                  && edges[2] == 3 * kSixteenth, edgesText(edges));
}

TEST(sequencing, step_sequencer_directions)
{
    // Clock edges every 100 samples, read the step index after each.
    // Directions: 0 forward, 1 backward, 2 pingpong.
    auto stepsFor = [](int direction) {
        ModuleRunner seq("step_sequencer");
        seq.setParam("steps", 3);
        seq.setParam("direction", float(direction));
        std::vector<float> clock(1000, 0.0f);
        for (int i = 0; i < 1000; ++i) clock[size_t(i)] = (i % 100) < 50 ? 1.0f : 0.0f;
        const auto out = seq.run(1000, { { "clock", clock } });
        std::vector<int> steps;
        for (int k = 0; k < 10; ++k) steps.push_back(int(std::lround(out[3][size_t(k * 100 + 10)] * 2)));
        return steps;
    };
    CHECK((stepsFor(0) == std::vector<int>{ 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 }));
    CHECK((stepsFor(1) == std::vector<int>{ 0, 2, 1, 0, 2, 1, 0, 2, 1, 0 }));
    CHECK((stepsFor(2) == std::vector<int>{ 0, 1, 2, 1, 0, 1, 2, 1, 0, 1 }));
}

TEST(sequencing, transport_bar_and_beat_pulses)
{
    const auto out = renderGlobal(mod("tr", "transport"), "tr.bar", "tr.beat", kBeat * 12, playing());
    const auto bars = risingEdges(out.left);
    const auto beats = risingEdges(out.right);
    CHECK_MSG((bars == std::vector<int>{ 0, 4 * kBeat, 8 * kBeat }), edgesText(bars));
    CHECK(beats.size() == 12);
    for (size_t k = 0; k < beats.size(); ++k) CHECK(beats[k] == int(k) * kBeat);

    // Pulses last 5 ms.
    int high = 0;
    for (int i = 0; i < kBeat; ++i) high += out.left[size_t(i)] > 0.5f;
    CHECK_NEAR(high, kRate * 0.005, 2);

    // In 3/4 the bar pulse comes every three beats.
    Transport waltz = playing();
    waltz.beatsPerBar = 3;
    const auto three = renderGlobal(mod("tr", "transport"), "tr.bar", "tr.beat", kBeat * 7, waltz);
    CHECK_MSG((risingEdges(three.left) == std::vector<int>{ 0, 3 * kBeat, 6 * kBeat }),
              edgesText(risingEdges(three.left)));
}
