#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <array>
#include <cmath>

// Complete voices: drum voices fired by a trigger, and a self-contained FM
// synth voice. Each replaces a small patch of oscillators and envelopes.
namespace OpenDaw::Modular {

namespace {

using namespace detail;

inline float finiteOr0(float v) { return std::isfinite(v) ? v : 0.0f; }

// Per-sample multiplier that falls to -60 dB in `seconds`, the usual meaning
// of a drum's decay time.
inline float decayCoef(float seconds, double sr)
{
    return static_cast<float>(std::exp(std::log(0.001) / (std::max(seconds, 0.001f) * sr)));
}

// Per-sample multiplier for a time constant (falls to 1/e in `seconds`).
inline float tauCoef(float seconds, double sr)
{
    return static_cast<float>(std::exp(-1.0 / (std::max(seconds, 0.0005f) * sr)));
}

// Rising-edge detector for gate/trigger inputs.
struct Trigger {
    bool high = false;
    bool rose(float v)
    {
        const bool now = v > 0.5f;
        const bool edge = now && !high;
        high = now;
        return edge;
    }
};

// Topology-preserving state-variable filter (lowpass, bandpass and highpass
// at once), stable for any cutoff below Nyquist.
struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    void set(double cutoffHz, double q, double sr)
    {
        const double fc = std::clamp(cutoffHz, 10.0, sr * 0.45);
        g = static_cast<float>(std::tan(kTwoPi * 0.5 * fc / sr));
        k = static_cast<float>(1.0 / std::max(q, 0.1));
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0.0f; }
    // Returns {low, band, high}.
    std::array<float, 3> process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, x - k * v1 - v2 };
    }
};

// Accent input: scales the hit's level; full level when unconnected.
inline float accentLevel(const bool* connected, int index, const float* in)
{
    return connected[index] ? clampf(finiteOr0(in[0]), 0.0f, 1.0f) : 1.0f;
}

// ── Kick ────────────────────────────────────────────────────────────────────

class Kick : public Module {
public:
    void prepare(double sr) override { sr_ = sr; }
    void reset() override { trig_ = {}; phase_ = 0.0; amp_ = pitchEnv_ = click_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float tune = p[0], sweep = p[1];
        const float pitchCoef = tauCoef(p[2], sr_), ampCoef = decayCoef(p[3], sr_);
        const float clickAmount = p[4], drive = 1.0f + 4.0f * p[5];
        const float driveNorm = std::tanh(drive);
        const float clickCoef = tauCoef(0.003f, sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                amp_ = pitchEnv_ = click_ = 1.0f;
                phase_ = 0.0;
            }
            const double hz = tune * std::exp2(clampf(finiteOr0(in[2][i]), -4.0f, 4.0f)
                                               + sweep * pitchEnv_);
            phase_ += hz / sr_;
            phase_ -= std::floor(phase_);
            const float body = static_cast<float>(std::sin(kTwoPi * phase_)) * amp_;
            const float x = body + clickAmount * click_ * noise_.next();
            out[0][i] = level_ * std::tanh(x * drive) / driveNorm;
            amp_ *= ampCoef;
            pitchEnv_ *= pitchCoef;
            click_ *= clickCoef;
        }
    }
private:
    double sr_ = 44100.0, phase_ = 0.0;
    float amp_ = 0.0f, pitchEnv_ = 0.0f, click_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
};

// ── Snare ───────────────────────────────────────────────────────────────────

class Snare : public Module {
public:
    void prepare(double sr) override { sr_ = sr; }
    void reset() override
    {
        trig_ = {}; noiseFilter_.reset();
        phase1_ = phase2_ = 0.0; tone_ = noiseEnv_ = pitchEnv_ = 0.0f;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float tune = p[0];
        const float toneCoef = decayCoef(p[1], sr_), noiseCoef = decayCoef(p[2], sr_);
        const float snappy = clampf(p[3], 0.0f, 1.0f);
        const float pitchCoef = tauCoef(0.01f, sr_);
        noiseFilter_.set(p[4], 0.7, sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                tone_ = noiseEnv_ = pitchEnv_ = 1.0f;
                phase1_ = phase2_ = 0.0;
            }
            // Two drum-head modes with a short pitch drop on the hit.
            const double hz = tune * std::exp2(clampf(finiteOr0(in[2][i]), -4.0f, 4.0f)
                                               + 0.5 * pitchEnv_);
            phase1_ += hz / sr_;
            phase2_ += hz * 1.47 / sr_;
            phase1_ -= std::floor(phase1_);
            phase2_ -= std::floor(phase2_);
            const float body = static_cast<float>(std::sin(kTwoPi * phase1_)
                                                  + 0.5 * std::sin(kTwoPi * phase2_)) / 1.5f;
            const float wires = noiseFilter_.process(noise_.next())[2];
            out[0][i] = level_ * clampf((1.0f - snappy) * body * tone_ + snappy * wires * noiseEnv_,
                                        -1.0f, 1.0f);
            tone_ *= toneCoef;
            noiseEnv_ *= noiseCoef;
            pitchEnv_ *= pitchCoef;
        }
    }
private:
    double sr_ = 44100.0, phase1_ = 0.0, phase2_ = 0.0;
    float tone_ = 0.0f, noiseEnv_ = 0.0f, pitchEnv_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
    Svf noiseFilter_;
};

// ── Hi-hat ──────────────────────────────────────────────────────────────────

class HiHat : public Module {
public:
    // The six detuned square waves of the classic analog hat.
    static constexpr std::array<double, 6> kMetalHz = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

    void prepare(double sr) override { sr_ = sr; }
    void reset() override { trig_ = {}; filter_.reset(); env_ = 0.0f; phases_.fill(0.0); }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float coef = decayCoef(p[0], sr_);
        filter_.set(p[1], 0.9, sr_);
        const float metal = clampf(p[2], 0.0f, 1.0f);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                env_ = 1.0f;
            }
            float squares = 0.0f;
            for (size_t k = 0; k < kMetalHz.size(); ++k) {
                phases_[k] += kMetalHz[k] * 1.7 / sr_;   // up into the hat's range
                phases_[k] -= std::floor(phases_[k]);
                squares += phases_[k] < 0.5 ? 1.0f : -1.0f;
            }
            const float source = metal * squares / 6.0f + (1.0f - metal) * noise_.next();
            out[0][i] = level_ * clampf(filter_.process(source)[2] * env_ * 1.5f, -1.0f, 1.0f);
            env_ *= coef;
        }
    }
private:
    double sr_ = 44100.0;
    std::array<double, 6> phases_{};
    float env_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
    Svf filter_;
};

// ── Clap ────────────────────────────────────────────────────────────────────

// A few band-passed noise bursts in quick succession (the "many hands"), the
// last of which opens into a longer tail.
class Clap : public Module {
public:
    static constexpr int kBursts = 4;

    void prepare(double sr) override { sr_ = sr; }
    void reset() override
    {
        trig_ = {}; filter_.reset();
        burst_ = tail_ = 0.0f; burstsLeft_ = 0; untilBurst_ = 0;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        filter_.set(p[0], 1.5, sr_);
        const float spread = clampf(p[1], 0.002f, 0.05f);
        const int spacing = std::max(1, static_cast<int>(spread * sr_));
        // Each burst has died ~35 dB by the next, so the hits stay distinct.
        const float burstCoef = tauCoef(spread / 4.0f, sr_);
        const float tailCoef = decayCoef(p[2], sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                burst_ = 1.0f;
                tail_ = 0.0f;
                burstsLeft_ = kBursts - 1;
                untilBurst_ = spacing;
            } else if (burstsLeft_ > 0 && --untilBurst_ <= 0) {
                burst_ = 1.0f;
                untilBurst_ = spacing;
                if (--burstsLeft_ == 0) tail_ = 0.6f;
            }
            const float env = std::max(burst_, tail_);
            // Unity-peak band-pass (k * band), boosted to make up for the narrow band.
            const float band = filter_.k * filter_.process(noise_.next())[1];
            out[0][i] = level_ * clampf(3.0f * band * env, -1.0f, 1.0f);
            burst_ *= burstCoef;
            tail_ *= tailCoef;
        }
    }
private:
    double sr_ = 44100.0;
    float burst_ = 0.0f, tail_ = 0.0f, level_ = 1.0f;
    int burstsLeft_ = 0, untilBurst_ = 0;
    Trigger trig_;
    NoiseGen noise_;
    Svf filter_;
};

// ── Tom ─────────────────────────────────────────────────────────────────────

class Tom : public Module {
public:
    void prepare(double sr) override { sr_ = sr; }
    void reset() override { trig_ = {}; phase_ = 0.0; amp_ = pitchEnv_ = click_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float tune = p[0], sweep = p[1];
        const float ampCoef = decayCoef(p[2], sr_);
        const float attack = clampf(p[3], 0.0f, 1.0f);
        // The drop is over within ~0.1 s whatever the decay, like a struck head.
        const float pitchCoef = tauCoef(0.03f, sr_), clickCoef = tauCoef(0.004f, sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                amp_ = pitchEnv_ = click_ = 1.0f;
                phase_ = 0.0;
            }
            const double hz = tune * std::exp2(clampf(finiteOr0(in[2][i]), -4.0f, 4.0f)
                                               + sweep * pitchEnv_);
            phase_ += hz / sr_;
            phase_ -= std::floor(phase_);
            const float body = static_cast<float>(std::sin(kTwoPi * phase_)) * amp_;
            out[0][i] = level_ * clampf(body + 0.5f * attack * click_ * noise_.next(), -1.0f, 1.0f);
            amp_ *= ampCoef;
            pitchEnv_ *= pitchCoef;
            click_ *= clickCoef;
        }
    }
private:
    double sr_ = 44100.0, phase_ = 0.0;
    float amp_ = 0.0f, pitchEnv_ = 0.0f, click_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
};

// ── Cymbal ──────────────────────────────────────────────────────────────────

// The hi-hat's metallic squares plus noise through a steep high-pass. A crash
// is one long noisy wash; a ride's wash dies early and leaves a resonant ping
// (and optionally a bell) ringing for the full decay.
class Cymbal : public Module {
public:
    static constexpr std::array<double, 6> kMetalHz = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
    static constexpr double kBellHz = 2400.0, kBellRatio = 1.47;

    void prepare(double sr) override { sr_ = sr; }
    void reset() override
    {
        trig_ = {}; hp1_.reset(); hp2_.reset(); ping_.reset();
        wash_ = ring_ = 0.0f; phases_.fill(0.0); bell1_ = bell2_ = 0.0;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float color = p[1];
        const bool ride = p[2] >= 0.5f;
        const float bell = clampf(p[3], 0.0f, 1.0f);
        hp1_.set(color, 0.7, sr_);
        hp2_.set(color, 0.7, sr_);
        ping_.set(color * 1.4, 8.0, sr_);
        const float ringCoef = decayCoef(p[0], sr_);
        const float washCoef = ride ? decayCoef(0.3f * p[0], sr_) : ringCoef;
        const float noiseMix = ride ? 0.15f : 0.5f;
        const float washGain = 1.4f, pingGain = ride ? 1.5f : 0.0f;
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                wash_ = ring_ = 1.0f;
            }
            float squares = 0.0f;
            for (size_t k = 0; k < kMetalHz.size(); ++k) {
                phases_[k] += kMetalHz[k] / sr_;
                phases_[k] -= std::floor(phases_[k]);
                squares += phases_[k] < 0.5 ? 1.0f : -1.0f;
            }
            const float source = (1.0f - noiseMix) * squares / 6.0f + noiseMix * noise_.next();
            const float high = hp2_.process(hp1_.process(source)[2])[2];
            const float ping = ping_.k * ping_.process(high)[1];
            bell1_ += kBellHz / sr_;
            bell2_ += kBellHz * kBellRatio / sr_;
            bell1_ -= std::floor(bell1_);
            bell2_ -= std::floor(bell2_);
            const float bellTone = static_cast<float>(std::sin(kTwoPi * bell1_)
                                                      + 0.6 * std::sin(kTwoPi * bell2_)) / 1.6f;
            const float x = washGain * high * wash_
                          + (pingGain * ping + 0.6f * bell * bellTone) * ring_;
            out[0][i] = level_ * clampf(x, -1.0f, 1.0f);
            wash_ *= washCoef;
            ring_ *= ringCoef;
        }
    }
private:
    double sr_ = 44100.0, bell1_ = 0.0, bell2_ = 0.0;
    std::array<double, 6> phases_{};
    float wash_ = 0.0f, ring_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
    Svf hp1_, hp2_, ping_;
};

// ── Rimshot ─────────────────────────────────────────────────────────────────

// A click and a high, quickly damped tone with a weaker low partial (the
// stick hitting the rim).
class Rimshot : public Module {
public:
    void prepare(double sr) override { sr_ = sr; }
    void reset() override { trig_ = {}; clickFilter_.reset(); phase1_ = phase2_ = 0.0; amp_ = click_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float tune = p[0];
        const float ampCoef = decayCoef(p[1], sr_);
        const float clickCoef = decayCoef(0.004f, sr_);
        clickFilter_.set(3000.0, 0.7, sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                amp_ = click_ = 1.0f;
                phase1_ = phase2_ = 0.0;
            }
            phase1_ += tune / sr_;
            phase2_ += tune * 0.27 / sr_;
            phase1_ -= std::floor(phase1_);
            phase2_ -= std::floor(phase2_);
            const float tone = static_cast<float>(std::sin(kTwoPi * phase1_)
                                                  + 0.4 * std::sin(kTwoPi * phase2_)) / 1.4f;
            const float click = clickFilter_.process(noise_.next())[2];
            out[0][i] = level_ * clampf(0.75f * tone * amp_ + 0.5f * click * click_, -1.0f, 1.0f);
            amp_ *= ampCoef;
            click_ *= clickCoef;
        }
    }
private:
    double sr_ = 44100.0, phase1_ = 0.0, phase2_ = 0.0;
    float amp_ = 0.0f, click_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    NoiseGen noise_;
    Svf clickFilter_;
};

// ── Cowbell ─────────────────────────────────────────────────────────────────

// Two square waves a little under a fifth apart (540 and 800 Hz at the
// default tune) through a band-pass.
class Cowbell : public Module {
public:
    static constexpr double kUpperRatio = 800.0 / 540.0;

    void prepare(double sr) override { sr_ = sr; }
    void reset() override { trig_ = {}; filter_.reset(); phase1_ = phase2_ = 0.0; env_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const double low = p[0], high = p[0] * kUpperRatio;
        const float coef = decayCoef(p[1], sr_);
        filter_.set(std::sqrt(low * high), 1.2, sr_);
        const double dt1 = low / sr_, dt2 = high / sr_;
        for (int i = 0; i < ctx.numSamples; ++i) {
            if (trig_.rose(in[0][i])) {
                level_ = accentLevel(connected, 1, in[1] + i);
                env_ = 1.0f;
                phase1_ = phase2_ = 0.0;
            }
            phase1_ += dt1;
            phase2_ += dt2;
            phase1_ -= std::floor(phase1_);
            phase2_ -= std::floor(phase2_);
            const double sq1 = (phase1_ < 0.5 ? 1.0 : -1.0) + polyBlep(phase1_, dt1)
                             - polyBlep(std::fmod(phase1_ + 0.5, 1.0), dt1);
            const double sq2 = (phase2_ < 0.5 ? 1.0 : -1.0) + polyBlep(phase2_, dt2)
                             - polyBlep(std::fmod(phase2_ + 0.5, 1.0), dt2);
            const float band = filter_.k * filter_.process(static_cast<float>(sq1 + sq2) * 0.5f)[1];
            out[0][i] = level_ * clampf(0.9f * band * env_, -1.0f, 1.0f);
            env_ *= coef;
        }
    }
private:
    double sr_ = 44100.0, phase1_ = 0.0, phase2_ = 0.0;
    float env_ = 0.0f, level_ = 1.0f;
    Trigger trig_;
    Svf filter_;
};

// ── FM voice ────────────────────────────────────────────────────────────────

// Two sine operators (modulator -> carrier) with an amplitude ADSR and a
// decaying brightness envelope on the FM index.
class FmVoice : public Module {
public:
    void prepare(double sr) override { sr_ = sr; }
    void reset() override
    {
        gateHigh_ = false; stage_ = Stage::Idle; amp_ = 0.0f; bright_ = 0.0f;
        carrier_ = modulator_ = 0.0; lastMod_ = 0.0f;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const double ratio = p[0];
        const float index = p[1], brightCoef = tauCoef(p[2], sr_), brightFloor = p[3];
        const float feedback = clampf(p[4], 0.0f, 1.0f);
        const float attackStep = 1.0f / std::max(1.0f, float(p[5] * sr_));
        const float decayC = tauCoef(std::max(p[6], 0.001f) / 3.0f, sr_);   // ~95% in `decay`
        const float sustain = clampf(p[7], 0.0f, 1.0f);
        const float releaseC = tauCoef(std::max(p[8], 0.001f) / 3.0f, sr_);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool gate = in[1][i] > 0.5f;
            if (gate && !gateHigh_) {
                stage_ = Stage::Attack;
                bright_ = 1.0f;
                velocity_ = connected[2] ? clampf(finiteOr0(in[2][i]), 0.0f, 1.0f) : 1.0f;
            } else if (!gate && gateHigh_) {
                stage_ = Stage::Release;
            }
            gateHigh_ = gate;
            switch (stage_) {
            case Stage::Attack:
                amp_ += attackStep;
                if (amp_ >= 1.0f) { amp_ = 1.0f; stage_ = Stage::Decay; }
                break;
            case Stage::Decay:   amp_ = sustain + (amp_ - sustain) * decayC; break;
            case Stage::Release:
                amp_ *= releaseC;
                if (amp_ < 1.0e-5f) { amp_ = 0.0f; stage_ = Stage::Idle; }
                break;
            case Stage::Idle: break;
            }

            const double hz = kC4Hz * std::exp2(clampf(finiteOr0(in[0][i]), -6.0f, 6.0f));
            modulator_ += hz * ratio / sr_;
            carrier_ += hz / sr_;
            modulator_ -= std::floor(modulator_);
            carrier_ -= std::floor(carrier_);
            const float envIndex = index * (brightFloor + (1.0f - brightFloor) * bright_)
                                 + 4.0f * finiteOr0(in[3][i]);
            const float mod = static_cast<float>(std::sin(kTwoPi * modulator_ + feedback * lastMod_));
            lastMod_ = mod;
            out[0][i] = velocity_ * amp_
                      * static_cast<float>(std::sin(kTwoPi * carrier_ + std::max(0.0f, envIndex) * mod));
            bright_ *= brightCoef;
        }
    }
private:
    enum class Stage { Idle, Attack, Decay, Release };
    double sr_ = 44100.0, carrier_ = 0.0, modulator_ = 0.0;
    float amp_ = 0.0f, bright_ = 0.0f, lastMod_ = 0.0f, velocity_ = 1.0f;
    bool gateHigh_ = false;
    Stage stage_ = Stage::Idle;
};

} // namespace

void addVoiceModules(std::vector<ModuleDef>& defs)
{
    using K = PortKind;

    defs.push_back({ "kick", "Kick drum: a sine whose pitch drops from 'sweep' octaves above "
        "'tune' on each trigger, with a click and drive. Fire it from voice_in.gate, a clock or "
        "a step_sequencer gate.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" },
          { "pitch", K::Pitch, "Shifts tune by octaves (1/octave)" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tune", 30.0f, 150.0f, 50.0f, "Hz", "The pitch it settles to"),
          param("sweep", 0.0f, 6.0f, 3.0f, "octaves", "How far above tune the hit starts"),
          param("pitch_decay", 0.005f, 0.3f, 0.04f, "s", "How fast the pitch drops"),
          param("decay", 0.05f, 3.0f, 0.5f, "s", "Time to fade to silence (-60 dB)"),
          param("click", 0.0f, 1.0f, 0.3f, "", "Noise click on the attack"),
          param("drive", 0.0f, 1.0f, 0.2f, "", "Soft saturation") },
        make<Kick>() });

    defs.push_back({ "snare", "Snare drum: two tuned drum-head tones plus high-passed noise "
        "for the wires. snappy blends from all tone (0) to all wires (1).",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" },
          { "pitch", K::Pitch, "Shifts tune by octaves (1/octave)" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tune", 100.0f, 400.0f, 180.0f, "Hz", "Drum-head pitch"),
          param("tone_decay", 0.02f, 1.0f, 0.15f, "s", "Fade time of the tone (-60 dB)"),
          param("noise_decay", 0.02f, 1.0f, 0.2f, "s", "Fade time of the wires (-60 dB)"),
          param("snappy", 0.0f, 1.0f, 0.6f, "", "0 = tone only, 1 = wires only"),
          param("color", 500.0f, 10000.0f, 2000.0f, "Hz", "High-pass on the wires; higher = thinner") },
        make<Snare>() });

    defs.push_back({ "hihat", "Hi-hat: six detuned square waves (metallic) and/or noise through "
        "a high-pass. Short decay = closed hat, long = open.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" } },
        { { "out", K::Audio, "-1..1" } },
        { param("decay", 0.01f, 2.0f, 0.08f, "s", "Fade time (-60 dB): ~0.05 closed, ~0.5 open"),
          param("color", 3000.0f, 15000.0f, 8000.0f, "Hz", "High-pass cutoff; higher = thinner"),
          param("metal", 0.0f, 1.0f, 0.5f, "", "0 = noise, 1 = metallic squares") },
        make<HiHat>() });

    defs.push_back({ "clap", "Hand clap: four band-passed noise bursts 'spread' seconds apart, "
        "the last opening into a tail that fades in 'decay'. Fire it on beats 2 and 4, often "
        "layered with a snare.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tone", 800.0f, 2500.0f, 1200.0f, "Hz", "Band-pass centre; higher = brighter"),
          param("spread", 0.005f, 0.02f, 0.01f, "s", "Gap between the bursts; wider = looser"),
          param("decay", 0.05f, 2.0f, 0.25f, "s", "Fade time of the tail (-60 dB)") },
        make<Clap>() });

    defs.push_back({ "tom", "Tom: a sine that drops 'sweep' octaves onto 'tune' on each "
        "trigger, with an optional noise attack. For a kit use several at different tunes, "
        "e.g. 80 Hz floor, 120 Hz low, 160 Hz mid, 220 Hz high.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" },
          { "pitch", K::Pitch, "Shifts tune by octaves (1/octave)" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tune", 60.0f, 400.0f, 120.0f, "Hz", "The pitch it settles to"),
          param("sweep", 0.0f, 2.0f, 0.5f, "octaves", "How far above tune the hit starts"),
          param("decay", 0.05f, 2.0f, 0.4f, "s", "Time to fade to silence (-60 dB)"),
          param("attack", 0.0f, 1.0f, 0.2f, "", "Noise on the attack; 0 = pure tone") },
        make<Tom>() });

    defs.push_back({ "cymbal", "Cymbal: metallic squares and noise through a steep high-pass. "
        "kind crash = a long noisy wash for accents at phrase starts; ride = a short wash "
        "over a ringing ping for steady eighths, with 'bell' for the ride bell.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" } },
        { { "out", K::Audio, "-1..1" } },
        { param("decay", 0.2f, 6.0f, 1.5f, "s", "Fade time (-60 dB); a ride's wash fades in a third of it"),
          param("color", 2000.0f, 12000.0f, 4000.0f, "Hz", "High-pass cutoff; higher = thinner"),
          choice("kind", { "crash", "ride" }, 0, "crash = noisy wash, ride = defined ping"),
          param("bell", 0.0f, 1.0f, 0.0f, "", "Bell tone at 2.4 kHz; for ride bell hits") },
        make<Cymbal>() });

    defs.push_back({ "rimshot", "Rimshot / side stick: a click and a short high tone. Use it "
        "for cross-stick backbeats or accents between snare hits.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tune", 1000.0f, 3000.0f, 1700.0f, "Hz", "Pitch of the tone"),
          param("decay", 0.005f, 0.2f, 0.04f, "s", "Fade time of the tone (-60 dB)") },
        make<Rimshot>() });

    defs.push_back({ "cowbell", "Cowbell: two square waves at 'tune' and 1.48 x tune (540 and "
        "800 Hz by default) through a band-pass, with a short decay. Good for Latin and "
        "disco patterns.",
        Section::Both,
        { { "trigger", K::Gate, "Hits on the rising edge" },
          { "accent", K::CV, "0..1 level of each hit; full when unconnected" } },
        { { "out", K::Audio, "-1..1" } },
        { param("tune", 300.0f, 1000.0f, 540.0f, "Hz", "Lower of the two tones"),
          param("decay", 0.02f, 1.0f, 0.15f, "s", "Fade time (-60 dB)") },
        make<Cowbell>() });

    defs.push_back({ "fm_voice", "A complete 2-operator FM synth voice: a sine modulator at "
        "'ratio' times the pitch modulates a sine carrier, with an amplitude ADSR and a "
        "brightness envelope that decays the FM index from 'index' towards index*'brightness'. "
        "Connect voice_in pitch/gate/velocity and send out to voice_out. Integer ratios are "
        "harmonic (1 = brassy, 2 = hollow), others bell-like.",
        Section::Both,
        { { "pitch", K::Pitch, "1/octave, 0 = C4" }, { "gate", K::Gate, "Note on while high" },
          { "velocity", K::CV, "0..1 level; full when unconnected" },
          { "index", K::CV, "Adds 4 to the FM index per unit" } },
        { { "out", K::Audio, "-1..1" } },
        { param("ratio", 0.25f, 16.0f, 2.0f, "", "Modulator frequency / carrier frequency"),
          param("index", 0.0f, 10.0f, 2.0f, "", "FM depth at the start of a note; 0 = pure sine"),
          param("index_decay", 0.01f, 5.0f, 0.5f, "s", "How fast the brightness falls"),
          param("brightness", 0.0f, 1.0f, 0.3f, "", "Index kept after the fall, as a fraction"),
          param("feedback", 0.0f, 1.0f, 0.0f, "", "Modulator feedback; adds grit"),
          param("attack", 0.0f, 5.0f, 0.005f, "s", ""), param("decay", 0.0f, 5.0f, 0.4f, "s", ""),
          param("sustain", 0.0f, 1.0f, 0.6f, "", "Level while held"),
          param("release", 0.0f, 5.0f, 0.3f, "s", "") },
        make<FmVoice>() });
}

} // namespace OpenDaw::Modular
