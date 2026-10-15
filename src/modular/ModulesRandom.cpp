#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace OpenDaw::Modular {

namespace {

using namespace detail;

// Distinct seeds per instance (constructors run on the message thread).
uint32_t nextSeed()
{
    static uint32_t counter = 0;
    uint32_t s = 0x9E3779B9u + 0x6D2B79F5u * ++counter;
    s ^= s >> 15; s *= 0x2C1B3C6Du; s ^= s >> 12;
    return s ? s : 1u;
}

inline float finiteOr0(float v) { return std::isfinite(v) ? v : 0.0f; }

// ── Sample & Hold ───────────────────────────────────────────────────────────

class SampleHold : public Module {
public:
    SampleHold() { noise_.state = nextSeed(); }
    void reset() override { held_ = 0.0f; lastGate_ = false; }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float*) override
    {
        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool g = in[1][i] > 0.5f;
            const float src = connected[0] ? in[0][i] : noise_.next();
            if (g && !lastGate_) held_ = clampf(finiteOr0(src), -16.0f, 16.0f);
            lastGate_ = g;
            out[0][i] = held_;
        }
    }
private:
    NoiseGen noise_;
    float held_ = 0.0f;
    bool lastGate_ = false;
};

// ── Quantizer ───────────────────────────────────────────────────────────────

constexpr int kNumScales = 13;
// Must match OpenDaw::Theory::parseScale for the shared names.
const int kScaleIntervals[kNumScales][13] = {  // -1 terminated
    { 0, 2, 4, 5, 7, 9, 11, -1 },              // major
    { 0, 2, 3, 5, 7, 8, 10, -1 },              // minor
    { 0, 2, 3, 5, 7, 9, 10, -1 },              // dorian
    { 0, 1, 3, 5, 7, 8, 10, -1 },              // phrygian
    { 0, 2, 4, 6, 7, 9, 11, -1 },              // lydian
    { 0, 2, 4, 5, 7, 9, 10, -1 },              // mixolydian
    { 0, 1, 3, 5, 6, 8, 10, -1 },              // locrian
    { 0, 2, 3, 5, 7, 8, 11, -1 },              // harmonic minor
    { 0, 2, 3, 5, 7, 9, 11, -1 },              // melodic minor
    { 0, 2, 4, 7, 9, -1 },                     // major pentatonic
    { 0, 3, 5, 7, 10, -1 },                    // minor pentatonic
    { 0, 3, 5, 6, 7, 10, -1 },                 // blues
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1 } // chromatic
};

class Quantizer : public Module {
public:
    Quantizer()
    {
        // offset_[scale][degree above root]: semitones to the nearest scale note
        // (searching down first, so ties go to the lower note).
        for (int s = 0; s < kNumScales; ++s) {
            std::array<bool, 12> in{};
            for (int k = 0; kScaleIntervals[s][k] >= 0; ++k) in[size_t(kScaleIntervals[s][k])] = true;
            for (int d = 0; d < 12; ++d) {
                int off = 0;
                for (int dist = 0; dist <= 6; ++dist) {
                    if (in[size_t(((d - dist) % 12 + 12) % 12)]) { off = -dist; break; }
                    if (in[size_t((d + dist) % 12)]) { off = dist; break; }
                }
                offset_[size_t(s)][size_t(d)] = off;
            }
        }
    }
    void prepare(double sr) override { pulseLen_ = std::max(1, int(sr * 0.002)); }
    void reset() override { lastNote_ = INT32_MIN; pulse_ = 0; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int root = std::clamp(static_cast<int>(p[0]), 0, 11);
        const int scale = std::clamp(static_cast<int>(p[1]), 0, kNumScales - 1);
        const int transpose = static_cast<int>(std::lround(clampf(p[2], -24.0f, 24.0f)));
        const auto& off = offset_[size_t(scale)];
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float pitch = clampf(finiteOr0(in[0][i]), -10.0f, 10.0f);
            // Nearest semitone (ties round down, with float slack), then nearest scale note.
            const int semi = static_cast<int>(std::ceil(pitch * 12.0f - 0.5f - 1e-4f));
            const int degree = ((semi - root) % 12 + 12) % 12;
            const int note = semi + off[size_t(degree)] + transpose;
            if (note != lastNote_) {
                if (lastNote_ != INT32_MIN) pulse_ = pulseLen_;
                lastNote_ = note;
            }
            out[0][i] = static_cast<float>(note) / 12.0f;
            out[1][i] = pulse_ > 0 ? 1.0f : 0.0f;
            if (pulse_ > 0) --pulse_;
        }
    }
private:
    std::array<std::array<int, 12>, kNumScales> offset_{};
    int pulseLen_ = 96;
    int lastNote_ = INT32_MIN;
    int pulse_ = 0;
};

// ── Random ──────────────────────────────────────────────────────────────────

class Random : public Module {
public:
    Random() { seed_ = nextSeed(); noise_.state = seed_; }
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override
    {
        phase_ = 0.0; from_ = to_ = 0.0f; gateOn_ = false; started_ = false;
        lastTrig_ = false; sinceTrig_ = 0; stepLen_ = sampleRate_;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const double rate = std::clamp(double(p[0]), 0.01, 50.0);
        const float smooth = clampf(p[1], 0.0f, 1.0f);
        const float prob = clampf(p[2], 0.0f, 1.0f);
        const bool triggered = connected[0];
        for (int i = 0; i < ctx.numSamples; ++i) {
            bool step = !started_ && !triggered;  // trigger mode waits for a real edge
            double frac = 0.0;  // position within the current step, 0..1
            if (triggered) {
                const bool g = in[0][i] > 0.5f;
                if (g && !lastTrig_) {
                    if (started_ && sinceTrig_ > 0) stepLen_ = double(sinceTrig_);
                    sinceTrig_ = 0;
                    step = true;
                }
                lastTrig_ = g;
                frac = std::min(1.0, double(sinceTrig_) / std::max(1.0, stepLen_));
                ++sinceTrig_;
                if (sinceTrig_ > (1 << 30)) sinceTrig_ = 1 << 30;
            } else {
                if (phase_ >= 1.0) { phase_ -= std::floor(phase_); step = true; }
                frac = phase_;
                phase_ += rate / sampleRate_;
            }
            if (step) {
                started_ = true;
                from_ = current_;
                to_ = noise_.next();
                gateOn_ = (noise_.next() * 0.5f + 0.5f) < prob;
                if (frac > 0.0 && triggered) frac = 0.0;
            }
            float t = smooth <= 0.0f ? 1.0f : static_cast<float>(std::min(1.0, frac / smooth));
            current_ = clampf(from_ + (to_ - from_) * t, -1.0f, 1.0f);
            out[0][i] = current_;
            out[1][i] = current_ * 0.5f + 0.5f;
            out[2][i] = (gateOn_ && frac < 0.5) ? 1.0f : 0.0f;
        }
    }
private:
    NoiseGen noise_;
    uint32_t seed_ = 1;
    double sampleRate_ = 44100.0;
    double phase_ = 0.0, stepLen_ = 44100.0;
    float from_ = 0.0f, to_ = 0.0f, current_ = 0.0f;
    bool gateOn_ = false, started_ = false, lastTrig_ = false;
    int sinceTrig_ = 0;
};

} // namespace

// Modules: sample_hold, quantizer, random.
void addRandomModules(std::vector<ModuleDef>& defs)
{
    using K = PortKind;

    defs.push_back({ "sample_hold", "Sample and hold: on each rising edge of 'trigger' it "
        "captures 'in' and holds it until the next trigger. With no 'in' cable it samples "
        "white noise (-1..1), giving classic random stepped CV. Feed a clock or gate into "
        "trigger; send 'out' to a quantizer for random melodies.",
        Section::Both,
        { { "in", K::CV, "Signal to sample; unconnected = internal white noise -1..1" },
          { "trigger", K::Gate, "Samples on each rising edge" } },
        { { "out", K::CV, "The held value" } }, {},
        make<SampleHold>() });

    defs.push_back({ "quantizer", "Snaps a pitch (1/octave, 0 = C4) to the nearest note of a "
        "scale (ties go to the lower note), then transposes it. Use it after random, "
        "sample_hold or an LFO to make melodies that stay in key. 'changed' gives a 2 ms "
        "trigger whenever the output note changes (e.g. to fire an adsr).",
        Section::Both,
        { { "pitch", K::Pitch, "1/octave, 0 = C4; 1/12 per semitone" } },
        { { "pitch", K::Pitch, "Quantized pitch, 1/octave" },
          { "changed", K::Gate, "2 ms pulse when the note changes" } },
        { choice("root", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0,
                 "Key of the scale"),
          choice("scale", { "major", "minor", "dorian", "phrygian", "lydian", "mixolydian",
                            "locrian", "harmonic minor", "melodic minor", "major pentatonic",
                            "minor pentatonic", "blues", "chromatic" }, 0, "Scale to snap to"),
          param("transpose", -24.0f, 24.0f, 0.0f, "semitones",
                "Added after snapping (whole semitones; may leave the scale)") },
        make<Quantizer>() });

    defs.push_back({ "random", "Random voltage generator. Makes a new random value at 'rate' "
        "Hz, or on each rising edge of 'trigger' when it is connected. smooth 0 = stepped, "
        "1 = glides all the way between values. 'gate' goes high for the first half of a "
        "step with chance 'probability' (random rhythms). Scale 'out' into a quantizer for "
        "random notes.",
        Section::Both,
        { { "trigger", K::Gate, "New value on each rising edge (overrides rate)" } },
        { { "out", K::CV, "-1..1" }, { "unipolar", K::CV, "0..1" },
          { "gate", K::Gate, "High for half a step when the coin flip succeeds" } },
        { param("rate", 0.01f, 50.0f, 1.0f, "Hz", "New values per second (without trigger)"),
          param("smooth", 0.0f, 1.0f, 0.0f, "", "0 = stepped, 1 = full glide between values"),
          param("probability", 0.0f, 1.0f, 0.5f, "", "Chance 0..1 the gate fires on each step") },
        make<Random>() });
}

} // namespace OpenDaw::Modular
