#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace OpenDaw::Modular {

namespace {

using namespace detail;

// Tiny bias so positions that land exactly on a grid line (in exact maths) are
// not pushed a sample late by floating-point rounding.
constexpr double kGridEps = 1e-9;

inline double fracPos(double x) { return x - std::floor(x); }

// ── Transport ───────────────────────────────────────────────────────────────

// Song position as gates and ramps. Pulses are derived from the position
// itself, so they land exactly on beat and bar starts.
class TransportModule : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override
    {
        wasPlaying_ = false; startLeft_ = 0; beatPhase_ = 0.0f; barPhase_ = 0.0f;
    }
    void process(const ProcessContext& ctx, const float* const*, const bool*,
                 float* const* out, const float*) override
    {
        const Transport& t = ctx.transport;
        const double bps = std::max(t.bpm, 1.0) / 60.0 / sampleRate_;
        const double bpb = std::max(t.beatsPerBar, 1);
        const double pulseBeats = 0.005 * std::max(t.bpm, 1.0) / 60.0;  // 5 ms in beats
        const int pulseSamples = std::max(1, static_cast<int>(0.005 * sampleRate_));

        if (t.playing && !wasPlaying_) startLeft_ = pulseSamples;
        if (!t.playing) startLeft_ = 0;
        wasPlaying_ = t.playing;

        for (int i = 0; i < ctx.numSamples; ++i) {
            float beatPulse = 0.0f, barPulse = 0.0f;
            if (t.playing) {
                const double b = t.beat + i * bps + kGridEps;
                const double fb = fracPos(b);
                const double fbar = fracPos(b / bpb);
                beatPhase_ = static_cast<float>(fb);
                barPhase_ = static_cast<float>(fbar);
                beatPulse = fb < pulseBeats ? 1.0f : 0.0f;
                barPulse = fbar * bpb < pulseBeats ? 1.0f : 0.0f;
            }
            out[0][i] = t.playing ? 1.0f : 0.0f;
            out[1][i] = beatPhase_;
            out[2][i] = barPhase_;
            out[3][i] = beatPulse;
            out[4][i] = barPulse;
            out[5][i] = startLeft_ > 0 ? 1.0f : 0.0f;
            if (startLeft_ > 0) --startLeft_;
        }
    }
private:
    double sampleRate_ = 44100.0;
    bool wasPlaying_ = false;
    int startLeft_ = 0;
    float beatPhase_ = 0.0f, barPhase_ = 0.0f;
};

// ── Clock ───────────────────────────────────────────────────────────────────

constexpr double kStepBeats[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,
                                  2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };

class Clock : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { freePos_ = 0.0; }
    void process(const ProcessContext& ctx, const float* const*, const bool*,
                 float* const* out, const float* p) override
    {
        const int div = std::clamp(static_cast<int>(p[0]), 0, 8);
        const double gateLen = clampf(p[1], 0.01f, 0.99f);
        const double swing = clampf(p[2], 0.0f, 0.75f);
        const bool sync = static_cast<int>(p[3]) == 0;
        const double rate = std::clamp(static_cast<double>(p[4]), 0.1, 50.0);
        const Transport& t = ctx.transport;
        const double bpm = std::max(t.bpm, 1.0);
        const double bps = bpm / 60.0 / sampleRate_;

        // Positions are measured in steps; one swing pair is two steps.
        const double offset = swing * 0.5;
        const double trigSteps = sync ? 0.002 * bpm / 60.0 / kStepBeats[div] : 0.002 * rate;

        for (int i = 0; i < ctx.numSamples; ++i) {
            double pos;
            if (sync) {
                if (!t.playing) { out[0][i] = out[1][i] = out[2][i] = 0.0f; continue; }
                pos = (t.beat + i * bps) / kStepBeats[div] + kGridEps;
            } else {
                pos = freePos_ + kGridEps;
                freePos_ += rate / sampleRate_;
                if (freePos_ >= 2.0) freePos_ -= 2.0;
            }
            const double pair = pos - 2.0 * std::floor(pos * 0.5);  // 0..2
            double start, len;
            if (pair < 1.0 + offset) { start = 0.0; len = 1.0 + offset; }
            else { start = 1.0 + offset; len = 1.0 - offset; }
            const double into = pair - start;
            const double phase = std::clamp(into / len, 0.0, 1.0);
            out[0][i] = phase < gateLen ? 1.0f : 0.0f;
            out[1][i] = into < trigSteps ? 1.0f : 0.0f;
            out[2][i] = static_cast<float>(phase);
        }
    }
private:
    double sampleRate_ = 44100.0;
    double freePos_ = 0.0;
};

// ── Step sequencer ──────────────────────────────────────────────────────────

constexpr int kMaxSteps = 16;

class StepSequencer : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override
    {
        cur_ = -1; forward_ = true; lastClock_ = false; lastReset_ = false;
        resetPending_ = false; endLeft_ = 0; noise_ = NoiseGen{};
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int steps = std::clamp(static_cast<int>(std::lround(p[0])), 1, kMaxSteps);
        const int dir = std::clamp(static_cast<int>(p[1]), 0, 3);
        const float* pitch = p + 2;
        const float* gates = p + 2 + kMaxSteps;
        const float* vels = p + 2 + 2 * kMaxSteps;
        const int endSamples = std::max(1, static_cast<int>(0.002 * sampleRate_));
        if (cur_ >= steps) cur_ = steps - 1;

        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool rst = in[1][i] > 0.5f;
            if (rst && !lastReset_) resetPending_ = true;
            lastReset_ = rst;

            const bool clk = in[0][i] > 0.5f;
            if (clk && !lastClock_) advance(steps, dir, endSamples);
            lastClock_ = clk;

            const int s = cur_ < 0 ? 0 : cur_;
            out[0][i] = clampf(pitch[s], -24.0f, 24.0f) / 12.0f;
            out[1][i] = (cur_ >= 0 && clk && gates[s] >= 0.5f) ? 1.0f : 0.0f;
            out[2][i] = clampf(vels[s], 0.0f, 1.0f);
            out[3][i] = steps > 1 ? static_cast<float>(s) / static_cast<float>(steps - 1) : 0.0f;
            out[4][i] = endLeft_ > 0 ? 1.0f : 0.0f;
            if (endLeft_ > 0) --endLeft_;
        }
    }
private:
    void advance(int steps, int dir, int endSamples)
    {
        if (cur_ < 0 || resetPending_) {
            cur_ = 0; forward_ = true; resetPending_ = false;
            return;
        }
        bool wrapped = false;
        switch (dir) {
        case 0:
            if (++cur_ >= steps) { cur_ = 0; wrapped = true; }
            break;
        case 1:
            if (--cur_ < 0) { cur_ = steps - 1; wrapped = true; }
            break;
        case 2:
            if (steps == 1) { cur_ = 0; wrapped = true; break; }
            if (forward_) {
                if (cur_ + 1 >= steps) { forward_ = false; cur_ = steps - 2; }
                else ++cur_;
            } else {
                if (cur_ - 1 < 0) { forward_ = true; cur_ = 1; wrapped = true; }
                else --cur_;
            }
            break;
        default: {
            const float r = noise_.next() * 0.5f + 0.5f;
            const int next = std::min(steps - 1, static_cast<int>(r * steps));
            wrapped = next <= cur_ && cur_ == steps - 1;
            cur_ = next;
            break;
        }
        }
        if (wrapped) endLeft_ = endSamples;
    }

    double sampleRate_ = 44100.0;
    int cur_ = -1;
    bool forward_ = true, lastClock_ = false, lastReset_ = false, resetPending_ = false;
    int endLeft_ = 0;
    NoiseGen noise_;
};

} // namespace

// Modules: transport, clock, step_sequencer.
void addSequencingModules(std::vector<ModuleDef>& defs)
{
    using K = PortKind;

    defs.push_back({ "transport", "The song's transport as signals: a playing gate, beat and "
        "bar ramps, and short pulses on each beat, each bar and at playback start. Use it to "
        "sync LFO resets, envelopes or sequencers to the song. Silent pulses while stopped.",
        Section::Global, {},
        { { "playing", K::Gate, "1 while the song plays" },
          { "beat_phase", K::CV, "0..1 ramp over each beat" },
          { "bar_phase", K::CV, "0..1 ramp over each bar" },
          { "beat", K::Gate, "5 ms pulse at each beat start" },
          { "bar", K::Gate, "5 ms pulse at each bar start" },
          { "start", K::Gate, "5 ms pulse when playback starts" } },
        {}, make<TransportModule>() });

    defs.push_back({ "clock", "Step clock. In sync mode steps follow the song grid at "
        "'division' (step k starts at beat k * step length) and outputs are 0 while stopped; "
        "in free mode it runs at 'rate' Hz all the time. Feed 'gate' or 'trigger' into a "
        "step_sequencer clock or an adsr gate.",
        Section::Both, {},
        { { "gate", K::Gate, "High for gate_length of each step" },
          { "trigger", K::Gate, "2 ms pulse at each step start" },
          { "step_phase", K::CV, "0..1 ramp over each step" } },
        { choice("division", { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4t", "1/8t",
                               "1/16t" }, 4, "Step length in sync mode (1/4 = one beat, t = triplet)"),
          param("gate_length", 0.01f, 0.99f, 0.5f, "", "Gate high time as a fraction of a step"),
          param("swing", 0.0f, 0.75f, 0.0f, "", "Delays every second step by swing * half a step"),
          choice("mode", { "sync", "free" }, 0, "sync = song tempo grid, free = 'rate' Hz"),
          param("rate", 0.1f, 50.0f, 4.0f, "Hz", "Steps per second in free mode") },
        make<Clock>() });

    std::vector<ParamDef> seqParams = {
        param("steps", 1.0f, 16.0f, 16.0f, "", "Sequence length in steps (integer)"),
        choice("direction", { "forward", "backward", "pingpong", "random" }, 0,
               "Playback order"),
    };
    for (int k = 1; k <= kMaxSteps; ++k)
        seqParams.push_back(param("pitch_" + std::to_string(k), -24.0f, 24.0f, 0.0f,
                                  "semitones", "Step " + std::to_string(k) + " pitch from C4"));
    for (int k = 1; k <= kMaxSteps; ++k)
        seqParams.push_back(choice("gate_" + std::to_string(k), { "off", "on" }, 1,
                                   "Step " + std::to_string(k) + " plays (on) or rests (off)"));
    for (int k = 1; k <= kMaxSteps; ++k)
        seqParams.push_back(param("velocity_" + std::to_string(k), 0.0f, 1.0f, 0.8f, "",
                                  "Step " + std::to_string(k) + " velocity 0..1"));

    defs.push_back({ "step_sequencer", "16-step pitch/gate/velocity sequencer. Each rising "
        "edge on 'clock' (e.g. clock.gate) advances one step; 'reset' makes the next clock "
        "edge go to step 1. pitch output is 1/octave (patch it into a vco pitch); gate is "
        "high while the clock is high and the step's gate is on (patch it into an adsr).",
        Section::Both,
        { { "clock", K::Gate, "Advance one step on each rising edge" },
          { "reset", K::Gate, "Rising edge: next clock goes to step 1" } },
        { { "pitch", K::Pitch, "Step pitch, 1/octave, 0 = C4" },
          { "gate", K::Gate, "Clock gate, masked by the step's gate" },
          { "velocity", K::CV, "Step velocity 0..1" },
          { "step", K::CV, "Current step index / (steps - 1), 0..1" },
          { "end", K::Gate, "2 ms pulse when the sequence wraps" } },
        std::move(seqParams), make<StepSequencer>() });
}

} // namespace OpenDaw::Modular
