#include "TestHarness.h"
#include "music/Theory.h"

using namespace OpenDaw::Tests;

TEST(random, quantizer_matches_theory)
{
    struct Case { const char* key; int root; int scale; };
    // root and scale are the quantizer's choice indices.
    const Case cases[] = { { "C major", 0, 0 }, { "A minor", 9, 1 }, { "D dorian", 2, 2 } };

    for (const auto& c : cases) {
        const auto scale = OpenDaw::Theory::parseScale(c.key);
        CHECK_MSG(scale.has_value(), c.key);
        if (!scale) continue;

        ModuleRunner q("quantizer");
        q.setParam("root", float(c.root));
        q.setParam("scale", float(c.scale));
        // Every semitone from C3 to C5, one sub-block each.
        const int lo = -12, hi = 12, block = OpenDaw::Modular::kSubBlock;
        std::vector<float> pitch;
        for (int semi = lo; semi <= hi; ++semi) pitch.insert(pitch.end(), size_t(block), semi / 12.0f);
        const auto out = q.run(int(pitch.size()), { { "pitch", pitch } });

        for (int semi = lo; semi <= hi; ++semi) {
            const int got = int(std::lround(out[0][size_t((semi - lo) * block + block - 1)] * 12.0f));
            const int expected = scale->snap(60 + semi) - 60;
            CHECK_MSG(got == expected, std::string(c.key) + ": " + str(double(semi)) + " -> "
                                           + str(double(got)) + ", Theory says " + str(double(expected)));
        }
    }
}

TEST(random, quantizer_rounds_and_transposes)
{
    ModuleRunner q("quantizer");
    q.setParam("transpose", 12.0f);
    // A quarter tone above E rounds to E (in C major), then up an octave.
    const auto out = q.run(64, { { "pitch", std::vector<float>(64, 4.4f / 12.0f) } });
    CHECK_NEAR(out[0][63] * 12.0f, 16.0, 1e-4);

    // 'changed' pulses when the note changes, not on the first note.
    std::vector<float> pitch(960, 0.0f);
    std::fill(pitch.begin() + 480, pitch.end(), 7.0f / 12.0f);
    ModuleRunner q2("quantizer");
    const auto changed = q2.run(960, { { "pitch", pitch } })[1];
    CHECK((risingEdges(changed) == std::vector<int>{ 480 }));
}

TEST(random, values_and_reset)
{
    // Stepped values at 50 Hz: one value per 960 samples.
    auto sequence = [](ModuleRunner& r) {
        const auto out = r.run(960 * 16);
        std::vector<float> values;
        for (int k = 0; k < 16; ++k) values.push_back(out[0][size_t(k * 960 + 480)]);
        return values;
    };
    ModuleRunner r("random");
    r.setParam("rate", 50.0f);
    const auto first = sequence(r);
    for (float v : first) CHECK(v >= -1.0f && v <= 1.0f);
    CHECK(std::adjacent_find(first.begin(), first.end()) == first.end());  // steps change

    r.module().reset();
    const auto afterReset = sequence(r);
    CHECK(first != afterReset);

    // Two instances don't repeat each other either.
    ModuleRunner other("random");
    other.setParam("rate", 50.0f);
    CHECK(sequence(other) != first);
}

TEST(random, triggered_and_probability)
{
    // With a trigger cable, values change only on its rising edges.
    std::vector<float> trig(4000, 0.0f);
    for (int i : { 0, 1000, 2000, 3000 }) std::fill(trig.begin() + i, trig.begin() + i + 10, 1.0f);
    ModuleRunner r("random");
    r.setParam("rate", 50.0f);
    const auto out = r.run(4000, { { "trigger", trig } })[0];
    for (int seg = 0; seg < 4; ++seg)
        for (int i = seg * 1000 + 1; i < (seg + 1) * 1000; ++i)
            if (out[size_t(i)] != out[size_t(seg * 1000)]) { CHECK_MSG(false, "changed at " + str(double(i))); break; }

    // probability 0 never gates, 1 always does (for the first half of each step).
    for (float prob : { 0.0f, 1.0f }) {
        ModuleRunner g("random");
        g.setParam("rate", 50.0f);
        g.setParam("probability", prob);
        const auto gate = g.run(960 * 10)[2];
        CHECK(risingEdges(gate).size() == (prob > 0.5f ? 10u : 0u));
    }
}

TEST(random, sample_and_hold)
{
    std::vector<float> in(2000), trig(2000, 0.0f);
    for (int i = 0; i < 2000; ++i) in[size_t(i)] = i / 2000.0f;
    std::fill(trig.begin() + 500, trig.begin() + 510, 1.0f);
    std::fill(trig.begin() + 1500, trig.begin() + 1510, 1.0f);
    ModuleRunner sh("sample_hold");
    const auto out = sh.run(2000, { { "in", in }, { "trigger", trig } })[0];
    CHECK(out[100] == 0.0f);
    CHECK_NEAR(out[1000], 500 / 2000.0, 1e-6);
    CHECK_NEAR(out[1999], 1500 / 2000.0, 1e-6);
}
