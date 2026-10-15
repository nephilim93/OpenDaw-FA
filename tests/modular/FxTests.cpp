#include "TestHarness.h"

using namespace OpenDaw::Tests;

namespace {

std::vector<float> impulse(int len)
{
    std::vector<float> v(size_t(len), 0.0f);
    v[0] = 1.0f;
    return v;
}

// Indices of samples above `threshold` in absolute value.
std::vector<int> peaks(const std::vector<float>& s, float threshold)
{
    std::vector<int> out;
    for (size_t i = 0; i < s.size(); ++i) if (std::abs(s[i]) > threshold) out.push_back(int(i));
    return out;
}

// A delay with the tone filter out of the way, wet only.
ModuleRunner cleanDelay(float timeMs, float feedback)
{
    ModuleRunner d("delay");
    d.setParam("time", timeMs);
    d.setParam("feedback", feedback);
    d.setParam("mix", 1.0f);
    d.setParam("tone", 20000.0f);
    return d;
}

} // namespace

TEST(fx, delay_impulse_arrives_on_time)
{
    auto d = cleanDelay(250.0f, 0.0f);
    const auto out = d.run(int(kRate), { { "left", impulse(int(kRate)) } });
    const int expected = int(kRate * 0.25);
    for (int ch = 0; ch < 2; ++ch) {
        const auto p = peaks(out[size_t(ch)], 1e-3f);
        CHECK_MSG(p.size() == 1 && p[0] == expected, "channel " + str(double(ch)));
        if (!p.empty()) CHECK_NEAR(out[size_t(ch)][size_t(p[0])], 1.0, 1e-6);
    }
}

TEST(fx, delay_feedback_halves_each_echo)
{
    auto d = cleanDelay(100.0f, 0.5f);
    const auto left = d.run(int(kRate), { { "left", impulse(int(kRate)) } })[0];
    const int step = int(kRate * 0.1);
    for (int k = 1; k <= 6; ++k) {
        const double expected = std::pow(0.5, k - 1);
        CHECK_NEAR(left[size_t(k * step)], expected, expected * 1e-4);
        CHECK(std::abs(left[size_t(k * step + step / 2)]) < 1e-6f);  // nothing between echoes
    }
}

TEST(fx, delay_tempo_sync)
{
    // 1/4 at 120 bpm is 500 ms whatever the time param says; 1/8d is 375 ms.
    OpenDaw::Modular::Transport t;
    t.bpm = 120.0;
    for (const auto& [sync, ms] : { std::pair{ 1, 500.0 }, std::pair{ 3, 375.0 } }) {
        auto d = cleanDelay(80.0f, 0.0f);
        d.setParam("sync", float(sync));
        const auto p = peaks(d.run(int(kRate), { { "left", impulse(int(kRate)) } }, t)[0], 1e-3f);
        CHECK_MSG(p.size() == 1 && p[0] == int(kRate * ms / 1000.0), "sync " + str(double(sync)));
    }
    // And it follows the tempo: 1/4 at 90 bpm is 666.7 ms.
    t.bpm = 90.0;
    auto d = cleanDelay(80.0f, 0.0f);
    d.setParam("sync", 1.0f);
    const auto out = d.run(int(kRate), { { "left", impulse(int(kRate)) } }, t)[0];
    const int at = int(std::max_element(out.begin(), out.end()) - out.begin());
    CHECK_NEAR(at, kRate * 2.0 / 3.0, 1.0);
}

TEST(fx, delay_ping_pong)
{
    auto d = cleanDelay(100.0f, 0.5f);
    d.setParam("ping_pong", 1.0f);
    const auto out = d.run(int(kRate * 0.5), { { "left", impulse(int(kRate * 0.5)) } });
    const int step = int(kRate * 0.1);
    // Echoes alternate: left, right, left...
    CHECK(std::abs(out[0][size_t(step)]) > 0.1f && std::abs(out[1][size_t(step)]) < 1e-6f);
    CHECK(std::abs(out[1][size_t(2 * step)]) > 0.1f && std::abs(out[0][size_t(2 * step)]) < 1e-6f);
    CHECK(std::abs(out[0][size_t(3 * step)]) > 0.05f && std::abs(out[1][size_t(3 * step)]) < 1e-6f);
}

TEST(fx, reverb_decays_and_stays_finite)
{
    ModuleRunner r("reverb");
    r.setParam("mix", 1.0f);
    r.setParam("size", 0.8f);
    const int len = int(kRate * 8);
    const auto out = r.run(len, { { "left", impulse(len) } });
    for (const auto& ch : out) {
        CHECK(allFinite(ch));
        const double early = rms(ch, 0, int(kRate * 0.5));
        const double mid = rms(ch, int(kRate * 1.5), int(kRate * 2.0));
        const double late = rms(ch, int(kRate * 7.5), len);
        CHECK_MSG(early > 1e-3, "early " + str(early));
        CHECK_MSG(mid < early * 0.5, "mid " + str(mid) + " early " + str(early));
        CHECK_MSG(late < early * 1e-3, "late " + str(late) + " early " + str(early));
    }
    // Pre-delay: nothing comes out before it.
    for (int i = 0; i < int(kRate * 0.01); ++i) CHECK(out[0][size_t(i)] == 0.0f);

    // Loud, sustained input at the largest size still stays bounded.
    ModuleRunner big("reverb");
    big.setParam("size", 1.0f);
    big.setParam("damping", 0.0f);
    big.setParam("mix", 1.0f);
    std::vector<float> loud(size_t(kRate * 4));
    for (size_t i = 0; i < loud.size(); ++i) loud[i] = (i / 50) % 2 ? 4.0f : -4.0f;
    for (const auto& ch : big.run(int(loud.size()), { { "left", loud } })) {
        CHECK(allFinite(ch));
        CHECK(*std::max_element(ch.begin(), ch.end()) <= 8.0f);
        CHECK(*std::min_element(ch.begin(), ch.end()) >= -8.0f);
    }
}
