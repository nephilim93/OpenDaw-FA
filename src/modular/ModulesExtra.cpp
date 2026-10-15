#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace OpenDaw::Modular {

namespace {

using namespace detail;

inline float flushDenormal(float v) { return std::abs(v) < 1.0e-15f || !std::isfinite(v) ? 0.0f : v; }
inline float finiteOr0(float v) { return std::isfinite(v) ? v : 0.0f; }

// ── Wavefolder ──────────────────────────────────────────────────────────────

class Wavefolder : public Module {
public:
    void prepare(double sr) override
    {
        dcCoef_ = static_cast<float>(std::exp(-kTwoPi * 5.0 / sr));  // ~5 Hz DC blocker
    }
    void reset() override { dcX_ = dcY_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const float drive = p[0], symmetry = clampf(p[1], -1.0f, 1.0f);
        const float mix = clampf(p[2], 0.0f, 1.0f);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float x = finiteOr0(in[0][i]);
            const float gain = clampf(drive + 4.0f * finiteOr0(in[1][i]), 0.0f, 20.0f);
            // Triangle fold: identity inside -1..1, mirrored back at each edge.
            const double u = double(x) * gain + symmetry;
            const double w = (u + 1.0) - 4.0 * std::floor((u + 1.0) * 0.25);  // 0..4
            const float folded = clampf(static_cast<float>(1.0 - std::abs(w - 2.0)), -1.0f, 1.0f);
            // An offset fold leaves DC behind; remove it so it doesn't thump a VCA.
            dcY_ = flushDenormal(folded - dcX_ + dcCoef_ * dcY_);
            dcX_ = folded;
            out[0][i] = x * (1.0f - mix) + dcY_ * mix;
        }
    }
private:
    float dcCoef_ = 0.9993f;
    float dcX_ = 0.0f, dcY_ = 0.0f;
};

// ── Ring modulator ──────────────────────────────────────────────────────────

class RingMod : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const float mix = clampf(p[0], 0.0f, 1.0f);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float a = finiteOr0(in[0][i]), b = finiteOr0(in[1][i]);
            out[0][i] = clampf(a * (1.0f - mix) + a * b * mix, -16.0f, 16.0f);
        }
    }
};

// ── Comb filter ─────────────────────────────────────────────────────────────

// Feedback comb tuned by pitch. The loop's damping filter is a two-tap
// average whose delay is subtracted from the line, so plucks stay in tune.
class CombFilter : public Module {
public:
    void prepare(double sr) override
    {
        sampleRate_ = sr;
        buf_.assign(size_t(sr / kMinHz) + 8, 0.0f);
        reset();
    }
    void reset() override
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        write_ = 0;
        last_ = 0.0f;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int size = int(buf_.size());
        if (size < 8) {
            for (int i = 0; i < ctx.numSamples; ++i) out[0][i] = in[0][i];
            return;
        }
        const float tune = p[0] / 12.0f;
        const float feedback = clampf(p[1], -0.95f, 0.95f);
        const float a = clampf(p[2], 0.0f, 1.0f) * 0.5f;  // second tap weight
        const float mix = clampf(p[3], 0.0f, 1.0f);
        const double maxHz = sampleRate_ * 0.45;

        for (int i = 0; i < ctx.numSamples; ++i) {
            const float x = finiteOr0(in[0][i]);
            const double octave = clampf(finiteOr0(in[1][i]) + tune, -10.0f, 10.0f);
            const double hz = std::clamp(kC4Hz * std::exp2(octave), kMinHz, maxHz);
            const double delay = std::clamp(sampleRate_ / hz - a, 1.0, double(size - 4));

            double rp = double(write_) - delay;
            if (rp < 0.0) rp += size;
            const int i0 = static_cast<int>(rp);
            const float frac = static_cast<float>(rp - i0);
            const int i1 = i0 + 1 >= size ? 0 : i0 + 1;
            const float delayed = buf_[size_t(i0)] + (buf_[size_t(i1)] - buf_[size_t(i0)]) * frac;
            const float damped = delayed * (1.0f - a) + last_ * a;
            last_ = delayed;

            const float y = clampf(flushDenormal(x + feedback * damped), -8.0f, 8.0f);
            buf_[size_t(write_)] = y;
            if (++write_ >= size) write_ = 0;
            out[0][i] = x * (1.0f - mix) + y * mix;
        }
    }
private:
    static constexpr double kMinHz = 20.0;
    double sampleRate_ = 44100.0;
    std::vector<float> buf_;
    int write_ = 0;
    float last_ = 0.0f;
};

// ── Chorus ──────────────────────────────────────────────────────────────────

class Chorus : public Module {
public:
    void prepare(double sr) override
    {
        sampleRate_ = sr;
        size_ = static_cast<int>(sr * 0.05) + 4;  // 50 ms covers base + spread + depth
        for (auto& b : buf_) b.assign(size_t(size_), 0.0f);
        reset();
    }
    void reset() override
    {
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        write_ = 0;
        phase_ = 0.0;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float* inR = connected[1] ? in[1] : in[0];
        if (size_ <= 0) {
            for (int i = 0; i < ctx.numSamples; ++i) { out[0][i] = in[0][i]; out[1][i] = inR[i]; }
            return;
        }
        const double rate = std::clamp(double(p[0]), 0.01, 10.0);
        const double depth = clampf(p[1], 0.0f, 1.0f) * 0.004 * sampleRate_;  // up to +/-4 ms
        const float mix = clampf(p[2], 0.0f, 1.0f);
        const int voices = 2 + std::clamp(static_cast<int>(p[3]), 0, 2);
        const double ms = sampleRate_ / 1000.0;
        const float norm = 1.0f / float(voices);

        for (int i = 0; i < ctx.numSamples; ++i) {
            const float dry[2] = { finiteOr0(in[0][i]), finiteOr0(inR[i]) };
            buf_[0][size_t(write_)] = dry[0];
            buf_[1][size_t(write_)] = dry[1];

            for (int ch = 0; ch < 2; ++ch) {
                float wet = 0.0f;
                for (int v = 0; v < voices; ++v) {
                    // Voices spread evenly around the LFO cycle; the right side
                    // runs a quarter cycle apart for width.
                    const double ph = phase_ + double(v) / voices + ch * 0.25;
                    const double d = (10.0 + 4.0 * v) * ms + depth * std::sin(kTwoPi * ph);
                    double rp = double(write_) - d;
                    if (rp < 0.0) rp += size_;
                    const int i0 = static_cast<int>(rp);
                    const float frac = static_cast<float>(rp - i0);
                    const int i1 = i0 + 1 >= size_ ? 0 : i0 + 1;
                    const auto& b = buf_[size_t(ch)];
                    wet += b[size_t(i0)] + (b[size_t(i1)] - b[size_t(i0)]) * frac;
                }
                out[ch][i] = dry[ch] * (1.0f - mix) + flushDenormal(wet * norm) * mix;
            }
            if (++write_ >= size_) write_ = 0;
            phase_ += rate / sampleRate_;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }
private:
    double sampleRate_ = 44100.0;
    std::array<std::vector<float>, 2> buf_;
    int size_ = 0, write_ = 0;
    double phase_ = 0.0;
};

// ── Envelope follower ───────────────────────────────────────────────────────

class EnvelopeFollower : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { env_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        auto coef = [&](float t) {
            return t <= 0.0001f ? 1.0f
                                : static_cast<float>(1.0 - std::exp(-1.0 / (t * sampleRate_)));
        };
        const float attack = coef(p[0]), release = coef(p[1]);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float x = std::min(std::abs(finiteOr0(in[0][i])), 1.0f);
            env_ = flushDenormal(env_ + (x - env_) * (x > env_ ? attack : release));
            out[0][i] = env_;
        }
    }
private:
    double sampleRate_ = 44100.0;
    float env_ = 0.0f;
};

// ── Comparator / logic / crossfade ──────────────────────────────────────────

// Schmitt trigger: needs to cross b by half the hysteresis to switch.
class Comparator : public Module {
public:
    void reset() override { high_ = false; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const float half = clampf(p[0], 0.0f, 2.0f) * 0.5f;
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float diff = finiteOr0(in[0][i]) - finiteOr0(in[1][i]);
            if (high_ ? diff < -half : diff > half) high_ = !high_;
            out[0][i] = high_ ? 1.0f : 0.0f;
        }
    }
private:
    bool high_ = false;
};

class Logic : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int op = std::clamp(static_cast<int>(p[0]), 0, 3);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool a = in[0][i] > 0.5f, b = in[1][i] > 0.5f;
            bool r;
            switch (op) {
            case 0:  r = a && b; break;
            case 1:  r = a || b; break;
            case 2:  r = a != b; break;
            default: r = !a; break;
            }
            out[0][i] = r ? 1.0f : 0.0f;
        }
    }
};

class Crossfade : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float m = clampf(p[0] + finiteOr0(in[2][i]), 0.0f, 1.0f);
            out[0][i] = finiteOr0(in[0][i]) * (1.0f - m) + finiteOr0(in[1][i]) * m;
        }
    }
};

// ── Clock divider ───────────────────────────────────────────────────────────

class ClockDivider : public Module {
public:
    void reset() override { count_ = -1; lastClock_ = false; lastReset_ = false; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float*) override
    {
        static constexpr int kDivs[4] = { 2, 3, 4, 8 };
        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool rst = in[1][i] > 0.5f;
            if (rst && !lastReset_) count_ = -1;
            lastReset_ = rst;

            const bool clk = in[0][i] > 0.5f;
            if (clk && !lastClock_) count_ = (count_ + 1) % 24;  // 24 = lcm(2, 3, 4, 8)
            lastClock_ = clk;

            // Square waves at 1/n of the clock rate: high for the first half of
            // each n input periods. Odd n splits the middle period at the clock
            // edge, giving 50% duty from a 50% clock.
            for (int d = 0; d < 4; ++d) {
                const int n = kDivs[d];
                const int k = count_ < 0 ? -1 : count_ % n;
                const bool high = k >= 0 && (2 * k < n - 1 || (2 * k == n - 1 && clk));
                out[d][i] = high ? 1.0f : 0.0f;
            }
        }
    }
private:
    int count_ = -1;  // -1: waiting for the first clock after a reset
    bool lastClock_ = false, lastReset_ = false;
};

// ── Euclidean rhythm ────────────────────────────────────────────────────────

constexpr int kMaxEuclidSteps = 32;

class Euclidean : public Module {
public:
    void reset() override
    {
        cur_ = -1; lastClock_ = false; lastReset_ = false; resetPending_ = false;
        builtSteps_ = builtPulses_ = -1;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int steps = std::clamp(static_cast<int>(std::lround(p[0])), 1, kMaxEuclidSteps);
        const int pulses = std::clamp(static_cast<int>(std::lround(p[1])), 0, steps);
        const int rotation = std::clamp(static_cast<int>(std::lround(p[2])), 0, kMaxEuclidSteps - 1);
        if (steps != builtSteps_ || pulses != builtPulses_) build(steps, pulses);
        if (cur_ >= steps) cur_ = steps - 1;

        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool rst = in[1][i] > 0.5f;
            if (rst && !lastReset_) resetPending_ = true;
            lastReset_ = rst;

            const bool clk = in[0][i] > 0.5f;
            if (clk && !lastClock_) {
                cur_ = (cur_ < 0 || resetPending_) ? 0 : (cur_ + 1) % steps;
                resetPending_ = false;
            }
            lastClock_ = clk;

            const bool hit = cur_ >= 0 && pattern_[size_t((cur_ + rotation) % steps)];
            out[0][i] = clk && hit ? 1.0f : 0.0f;
        }
    }
private:
    // Bjorklund / Toussaint: start with `pulses` groups "1" and the rest "0",
    // then keep appending the trailing groups onto the leading ones until at
    // most one trailing group is left. Fixed arrays: no allocation here.
    void build(int steps, int pulses)
    {
        builtSteps_ = steps;
        builtPulses_ = pulses;
        pattern_.fill(0);
        if (pulses == 0) return;

        Group front, back;
        front.bits[0] = 1; front.len = 1;
        back.bits[0] = 0; back.len = 1;
        int numFront = pulses, numBack = steps - pulses;
        while (numBack > 1) {
            Group joined = front;
            for (int k = 0; k < back.len; ++k) joined.bits[size_t(joined.len++)] = back.bits[size_t(k)];
            const int joinedCount = std::min(numFront, numBack);
            if (numFront > numBack) back = front;  // leftover leading groups trail now
            numBack = std::abs(numFront - numBack);
            numFront = joinedCount;
            front = joined;
        }
        int pos = 0;
        for (int g = 0; g < numFront; ++g)
            for (int k = 0; k < front.len; ++k) pattern_[size_t(pos++)] = front.bits[size_t(k)];
        for (int g = 0; g < numBack; ++g)
            for (int k = 0; k < back.len; ++k) pattern_[size_t(pos++)] = back.bits[size_t(k)];
    }

    struct Group { std::array<uint8_t, kMaxEuclidSteps> bits{}; int len = 0; };
    std::array<uint8_t, kMaxEuclidSteps> pattern_{};
    int builtSteps_ = -1, builtPulses_ = -1;
    int cur_ = -1;
    bool lastClock_ = false, lastReset_ = false, resetPending_ = false;
};

} // namespace

// Modules: wavefolder, ring_mod, comb_filter, chorus, envelope_follower,
// comparator, logic, crossfade, clock_divider, euclidean.
void addExtraModules(std::vector<ModuleDef>& defs)
{
    using K = PortKind;

    defs.push_back({ "wavefolder", "West-coast wavefolder: amplifies the signal by 'drive' and "
        "folds anything beyond -1..1 back on itself, adding bright, hollow harmonics. drive 1 "
        "leaves a -1..1 signal clean; 3-6 is rich; sweep the fold input with an adsr or lfo for "
        "timbre movement. Works best on a sine or triangle vco.",
        Section::Both,
        { { "in", K::Audio, "" }, { "fold", K::CV, "Adds 4 to drive per unit (e.g. adsr 0..1 sweeps drive up by 4)" } },
        { { "out", K::Audio, "-1..1" } },
        { param("drive", 0.0f, 10.0f, 2.0f, "", "Gain before folding; more = more folds and brighter"),
          param("symmetry", -1.0f, 1.0f, 0.0f, "", "Offset before folding; non-zero adds even harmonics"),
          param("mix", 0.0f, 1.0f, 1.0f, "", "0 = dry, 1 = folded only") },
        make<Wavefolder>() });

    defs.push_back({ "ring_mod", "Ring modulator: out = a * b. Two tones give their sum and "
        "difference frequencies (metallic, bell-like, inharmonic). With an lfo or envelope on b "
        "it acts as tremolo/VCA.",
        Section::Both,
        { { "a", K::Audio, "Carrier" }, { "b", K::Audio, "Modulator" } },
        { { "out", K::Audio, "" } },
        { param("mix", 0.0f, 1.0f, 1.0f, "", "0 = a unchanged, 1 = a * b only") },
        make<RingMod>() });

    defs.push_back({ "comb_filter", "Tuned feedback comb filter / Karplus-Strong string. It "
        "resonates at the pitch input (1/octave, 0 = C4) plus 'tune'. For a plucked string, "
        "feed it a short burst (noise through a vca with a fast adsr: attack 0, decay ~0.01 s, "
        "sustain 0) and patch voice_in.pitch into pitch; feedback sets the ring time, damping "
        "darkens it as it decays. Negative feedback sounds hollow and an octave lower.",
        Section::Both,
        { { "in", K::Audio, "Excitation" }, { "pitch", K::Pitch, "1/octave, 0 = C4" } },
        { { "out", K::Audio, "" } },
        { param("tune", -24.0f, 24.0f, 0.0f, "semitones", "Pitch offset"),
          param("feedback", -0.95f, 0.95f, 0.9f, "", "Resonance; near +/-0.95 rings for seconds, 0 = a single echo"),
          param("damping", 0.0f, 1.0f, 0.5f, "", "High-frequency loss per cycle; higher = duller, shorter pluck"),
          param("mix", 0.0f, 1.0f, 1.0f, "", "0 = dry, 1 = filtered only") },
        make<CombFilter>() });

    defs.push_back({ "chorus", "Stereo chorus: several slowly modulated short delays thicken "
        "and widen the sound. Global section only: put it after 'voices', before 'output' "
        "(or before a delay/reverb).",
        Section::Global,
        { { "left", K::Audio, "" }, { "right", K::Audio, "Optional; left is used if unconnected" } },
        { { "left", K::Audio, "" }, { "right", K::Audio, "" } },
        { param("rate", 0.05f, 5.0f, 0.8f, "Hz", "Modulation speed; above ~2 Hz it turns into vibrato"),
          param("depth", 0.0f, 1.0f, 0.5f, "", "Modulation amount (1 = +/-4 ms)"),
          param("mix", 0.0f, 1.0f, 0.5f, "", "0 = dry, 0.5 = classic chorus, 1 = wet only"),
          choice("voices", { "2", "3", "4" }, 0, "Delayed copies per side; more = lusher") },
        make<Chorus>() });

    defs.push_back({ "envelope_follower", "Tracks the loudness of a signal as a 0..1 CV. "
        "Use it to make one sound modulate another (e.g. a drum loop opening a filter, ducking).",
        Section::Both,
        { { "in", K::Audio, "" } },
        { { "out", K::CV, "0..1, follows the input's peak level" } },
        { param("attack", 0.0f, 1.0f, 0.005f, "s", "How fast it rises"),
          param("release", 0.0f, 5.0f, 0.15f, "s", "How fast it falls") },
        make<EnvelopeFollower>() });

    defs.push_back({ "comparator", "Gate that is high while a > b. Leave b unconnected to "
        "compare against 0 (an lfo becomes a square gate); patch a scale module's offset into "
        "b for another threshold. Hysteresis stops noisy signals chattering.",
        Section::Both,
        { { "a", K::CV, "" }, { "b", K::CV, "Threshold; 0 if unconnected" } },
        { { "gate", K::Gate, "1 while a > b" } },
        { param("hysteresis", 0.0f, 1.0f, 0.01f, "", "a must pass b by half this to switch") },
        make<Comparator>() });

    defs.push_back({ "logic", "Combines two gates (high above 0.5): and, or, xor, or not "
        "(inverts a, ignores b). Use it to combine rhythms or gate one pattern by another.",
        Section::Both,
        { { "a", K::Gate, "" }, { "b", K::Gate, "" } },
        { { "out", K::Gate, "0 or 1" } },
        { choice("op", { "and", "or", "xor", "not" }, 0, "Logic operation") },
        make<Logic>() });

    defs.push_back({ "crossfade", "Linear blend between two signals (audio or CV): 0 = a, "
        "1 = b. The mix input adds to the mix param, so an lfo or envelope can morph between "
        "two oscillators or two filters.",
        Section::Both,
        { { "a", K::Audio, "" }, { "b", K::Audio, "" }, { "mix", K::CV, "Added to the mix param" } },
        { { "out", K::Audio, "" } },
        { param("mix", 0.0f, 1.0f, 0.5f, "", "0 = a only, 0.5 = equal, 1 = b only") },
        make<Crossfade>() });

    defs.push_back({ "clock_divider", "Divides a clock: square gates at 1/2, 1/3, 1/4 and 1/8 "
        "of the clock rate, all rising together on the first clock. Feed it clock.gate (or "
        "transport.beat) to get slower rhythms for sequencers, envelopes or lfo resets.",
        Section::Both,
        { { "clock", K::Gate, "Counts rising edges" },
          { "reset", K::Gate, "Rising edge: the next clock is count 1 again" } },
        { { "div_2", K::Gate, "Every 2nd clock" }, { "div_3", K::Gate, "Every 3rd clock" },
          { "div_4", K::Gate, "Every 4th clock" }, { "div_8", K::Gate, "Every 8th clock" } },
        {}, make<ClockDivider>() });

    defs.push_back({ "euclidean", "Euclidean rhythm: spreads 'pulses' hits as evenly as "
        "possible over 'steps' (Bjorklund), advancing one step per clock rising edge. E.g. "
        "steps 8 pulses 3 = tresillo x..x..x., steps 16 pulses 5 = x..x..x..x..x..... gate is "
        "high while the clock is high on a hit step; patch it into an adsr gate.",
        Section::Both,
        { { "clock", K::Gate, "Advance one step on each rising edge" },
          { "reset", K::Gate, "Rising edge: next clock goes to step 1" } },
        { { "gate", K::Gate, "Clock gate on hit steps" } },
        { param("steps", 1.0f, 32.0f, 16.0f, "", "Pattern length (integer)"),
          param("pulses", 0.0f, 32.0f, 4.0f, "", "Hits per pattern (integer, at most steps)"),
          param("rotation", 0.0f, 31.0f, 0.0f, "", "Rotates the pattern: step 1 plays pattern step 1 + rotation") },
        make<Euclidean>() });
}

} // namespace OpenDaw::Modular
