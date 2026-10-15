#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace OpenDaw::Modular {

namespace {

using namespace detail;

// ── Voice In / Voice Out / Voices / Output ──────────────────────────────────

class VoiceIn : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { glided_ = 0.0f; first_ = true; }
    void process(const ProcessContext& ctx, const float* const*, const bool*,
                 float* const* out, const float* p) override
    {
        const VoiceState v = ctx.voice ? *ctx.voice : VoiceState{};
        const float glide = p[0];
        const float coef = glide <= 0.0005f
            ? 1.0f : static_cast<float>(1.0 - std::exp(-1.0 / (glide * sampleRate_)));
        if (first_) { glided_ = v.pitch; first_ = false; }
        for (int i = 0; i < ctx.numSamples; ++i) {
            glided_ += (v.pitch - glided_) * coef;
            out[0][i] = glided_;
            out[1][i] = v.gate;
            out[2][i] = v.velocity;
            out[3][i] = v.aftertouch;
            out[4][i] = ctx.modWheel;
        }
    }
private:
    double sampleRate_ = 44100.0;
    float glided_ = 0.0f;
    bool first_ = true;
};

// Writes its inputs into the summed voice mix (a mono patch fills both sides).
class VoiceOut : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const*, const float*) override
    {
        if (!ctx.voiceOut[0]) return;
        float* left = ctx.voiceOut[0];
        float* right = ctx.voiceOut[1];
        const float* r = connected[1] ? in[1] : in[0];
        for (int i = 0; i < ctx.numSamples; ++i) {
            left[i] += in[0][i];
            right[i] += r[i];
        }
    }
};

class Voices : public Module {
public:
    void process(const ProcessContext& ctx, const float* const*, const bool*,
                 float* const* out, const float*) override
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < ctx.numSamples; ++i)
                out[ch][i] = ctx.voiceMix[ch] ? ctx.voiceMix[ch][i] : 0.0f;
    }
};

// The patch's final output. The runtime reads its inputs (see CompiledPatch).
class Output : public Module {
public:
    void process(const ProcessContext&, const float* const*, const bool*,
                 float* const*, const float*) override {}
};

// ── Sound sources ───────────────────────────────────────────────────────────

class Vco : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { phase_ = 0.0; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int wave = static_cast<int>(p[0]);
        const float baseOct = p[1] + (p[2] + p[3] / 100.0f) / 12.0f;
        const float pwBase = p[4], fmDepth = p[5], level = p[6];
        const double maxHz = sampleRate_ * 0.45;

        for (int i = 0; i < ctx.numSamples; ++i) {
            const double octave = in[0][i] + baseOct + in[1][i] * fmDepth;
            const double hz = std::clamp(kC4Hz * std::exp2(octave), 0.1, maxHz);
            const double dt = hz / sampleRate_;
            const double t = phase_;

            double v = 0.0;
            switch (wave) {
            case 0:  v = 2.0 * t - 1.0 - polyBlep(t, dt); break;                  // saw
            case 1: {                                                              // square
                const double pw = std::clamp(pwBase + in[2][i] * 0.5, 0.02, 0.98);
                v = (t < pw ? 1.0 : -1.0) + polyBlep(t, dt)
                    - polyBlep(std::fmod(t + 1.0 - pw, 1.0), dt);
                break;
            }
            case 2:  v = 2.0 * std::abs(2.0 * t - 1.0) - 1.0; break;              // triangle
            default: v = std::sin(kTwoPi * t); break;                              // sine
            }
            out[0][i] = static_cast<float>(v) * level;

            phase_ += dt;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }
private:
    double sampleRate_ = 44100.0;
    double phase_ = 0.0;
};

class Noise : public Module {
public:
    void reset() override { b0_ = b1_ = b2_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const*, const bool*,
                 float* const* out, const float* p) override
    {
        const bool pink = p[0] >= 0.5f;
        const float level = p[1];
        for (int i = 0; i < ctx.numSamples; ++i) {
            float w = noise_.next();
            if (pink) {  // Paul Kellet's economy pink filter
                b0_ = 0.99765f * b0_ + w * 0.0990460f;
                b1_ = 0.96300f * b1_ + w * 0.2965164f;
                b2_ = 0.57000f * b2_ + w * 1.0526913f;
                w = (b0_ + b1_ + b2_ + w * 0.1848f) * 0.25f;
            }
            out[0][i] = w * level;
        }
    }
private:
    NoiseGen noise_;
    float b0_ = 0.0f, b1_ = 0.0f, b2_ = 0.0f;
};

// ── Processing ──────────────────────────────────────────────────────────────

// State-variable filter (Zavalishin TPT form): stable under fast modulation.
class Vcf : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { ic1_ = ic2_ = 0.0; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int mode = static_cast<int>(p[0]);
        const float cutoff = p[1], resonance = clampf(p[2], 0.0f, 1.0f);
        const float depth = p[3], keyTrack = p[4];
        const double k = 2.0 - 1.96 * resonance;
        const double maxHz = sampleRate_ * 0.45;

        for (int i = 0; i < ctx.numSamples; ++i) {
            const double hz = std::clamp(
                double(cutoff) * std::exp2(double(in[1][i] * depth + in[2][i] * keyTrack)),
                20.0, maxHz);
            const double g = std::tan(3.141592653589793 * hz / sampleRate_);
            const double a1 = 1.0 / (1.0 + g * (g + k));
            const double a2 = g * a1, a3 = g * a2;

            const double x = in[0][i];
            const double v3 = x - ic2_;
            const double v1 = a1 * ic1_ + a2 * v3;
            const double v2 = ic2_ + a2 * ic1_ + a3 * v3;
            ic1_ = 2.0 * v1 - ic1_;
            ic2_ = 2.0 * v2 - ic2_;

            double y = v2;                                  // low pass
            if (mode == 1) y = x - k * v1 - v2;             // high pass
            else if (mode == 2) y = v1;                     // band pass
            out[0][i] = static_cast<float>(y);
        }
    }
private:
    double sampleRate_ = 44100.0;
    double ic1_ = 0.0, ic2_ = 0.0;
};

class Vca : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float gain = p[0];
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float cv = connected[1] ? in[1][i] : 1.0f;
            out[0][i] = in[0][i] * gain * cv;
        }
    }
};

class Mixer : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        for (int i = 0; i < ctx.numSamples; ++i)
            out[0][i] = (in[0][i] * p[0] + in[1][i] * p[1] + in[2][i] * p[2]
                         + in[3][i] * p[3]) * p[4];
    }
};

// Attenuverter + offset: out = in * scale + offset.
class Scale : public Module {
public:
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        for (int i = 0; i < ctx.numSamples; ++i)
            out[0][i] = in[0][i] * p[0] + p[1];
    }
};

class Slew : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { y_ = 0.0f; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        auto coef = [&](float t) {
            return t <= 0.0005f ? 1.0f
                                : static_cast<float>(1.0 - std::exp(-1.0 / (t * sampleRate_)));
        };
        const float rise = coef(p[0]), fall = coef(p[1]);
        for (int i = 0; i < ctx.numSamples; ++i) {
            const float x = in[0][i];
            y_ += (x - y_) * (x > y_ ? rise : fall);
            out[0][i] = y_;
        }
    }
private:
    double sampleRate_ = 44100.0;
    float y_ = 0.0f;
};

// ── Modulation ──────────────────────────────────────────────────────────────

class Adsr : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { env_ = 0.0; stage_ = Stage::Idle; lastGate_ = false; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        auto coef = [&](float seconds, double ratio) {
            if (seconds <= 0.0005f) return 1.0;
            return 1.0 - std::exp(-std::log((1.0 + ratio) / ratio) / (seconds * sampleRate_));
        };
        const double attack = coef(p[0], 0.3), decay = coef(p[1], 0.001);
        const double sustain = clampf(p[2], 0.0f, 1.0f), release = coef(p[3], 0.001);

        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool gate = in[0][i] > 0.5f;
            if (gate && !lastGate_) stage_ = Stage::Attack;
            else if (!gate && lastGate_ && stage_ != Stage::Idle) stage_ = Stage::Release;
            lastGate_ = gate;

            switch (stage_) {
            case Stage::Attack:
                env_ += (1.3 - env_) * attack;
                if (env_ >= 1.0) { env_ = 1.0; stage_ = Stage::Decay; }
                break;
            case Stage::Decay:
                env_ += (sustain - env_) * decay;
                break;
            case Stage::Release:
                env_ += (0.0 - env_) * release;
                if (env_ < 1e-5) { env_ = 0.0; stage_ = Stage::Idle; }
                break;
            case Stage::Idle:
                break;
            }
            out[0][i] = static_cast<float>(env_);
        }
    }
private:
    enum class Stage { Idle, Attack, Decay, Release };
    double sampleRate_ = 44100.0;
    double env_ = 0.0;
    Stage stage_ = Stage::Idle;
    bool lastGate_ = false;
};

class Lfo : public Module {
public:
    void prepare(double sr) override { sampleRate_ = sr; }
    void reset() override { phase_ = 0.0; held_ = 0.0f; lastReset_ = false; }
    void process(const ProcessContext& ctx, const float* const* in, const bool*,
                 float* const* out, const float* p) override
    {
        const int wave = static_cast<int>(p[0]);
        const float rate = p[1];
        for (int i = 0; i < ctx.numSamples; ++i) {
            const bool resetGate = in[1][i] > 0.5f;
            if (resetGate && !lastReset_) phase_ = 0.0;
            lastReset_ = resetGate;

            const double t = phase_;
            float v = 0.0f;
            switch (wave) {
            case 0:  v = static_cast<float>(std::sin(kTwoPi * t)); break;
            case 1:  v = static_cast<float>(2.0 * std::abs(2.0 * t - 1.0) - 1.0); break;
            case 2:  v = static_cast<float>(2.0 * t - 1.0); break;
            case 3:  v = t < 0.5 ? 1.0f : -1.0f; break;
            default: v = held_; break;  // random step
            }
            out[0][i] = v;
            out[1][i] = v * 0.5f + 0.5f;

            const double hz = std::clamp(rate * std::exp2(in[0][i] * 4.0), 0.001, 200.0);
            phase_ += hz / sampleRate_;
            if (phase_ >= 1.0) { phase_ -= 1.0; held_ = noise_.next(); }
        }
    }
private:
    double sampleRate_ = 44100.0;
    double phase_ = 0.0;
    float held_ = 0.0f;
    bool lastReset_ = false;
    NoiseGen noise_;
};

std::vector<ModuleDef> buildRegistry()
{
    using K = PortKind;
    std::vector<ModuleDef> defs;

    defs.push_back({ "voice_in", "Per-note signals for this voice: pitch, gate, velocity, "
        "aftertouch and the mod wheel. The starting point of every voice patch.",
        Section::Voice, {},
        { { "pitch", K::Pitch, "1/octave, 0 = C4" }, { "gate", K::Gate, "1 while the key is held" },
          { "velocity", K::CV, "0..1" }, { "aftertouch", K::CV, "0..1" },
          { "mod", K::CV, "Mod wheel, 0..1" } },
        { param("glide", 0.0f, 2.0f, 0.0f, "s", "Portamento time between notes") },
        make<VoiceIn>() });

    defs.push_back({ "voice_out", "Sends this voice's sound to the global 'voices' module. "
        "Connect a mono signal to left only to use it on both sides.",
        Section::Voice,
        { { "left", K::Audio, "" }, { "right", K::Audio, "Optional" } }, {}, {},
        make<VoiceOut>() });

    defs.push_back({ "voices", "The sum of every voice's voice_out, for global processing.",
        Section::Global, {},
        { { "left", K::Audio, "" }, { "right", K::Audio, "" } }, {}, make<Voices>() });

    defs.push_back({ "output", "The patch's audio output to the track. Connect a mono "
        "signal to left only to use it on both sides.",
        Section::Global,
        { { "left", K::Audio, "" }, { "right", K::Audio, "Optional" } }, {},
        { param("volume", 0.0f, 1.0f, 0.8f, "", "Output level") },
        make<Output>() });

    defs.push_back({ "vco", "Oscillator. Pitch input follows 1/octave; fm adds exponential "
        "frequency modulation; pw modulates the square wave's pulse width.",
        Section::Both,
        { { "pitch", K::Pitch, "1/octave, 0 = C4" }, { "fm", K::CV, "Exponential FM, scaled by fm_depth" },
          { "pw", K::CV, "Pulse width modulation (square only)" } },
        { { "out", K::Audio, "" } },
        { choice("wave", { "saw", "square", "triangle", "sine" }, 0, "Waveform"),
          param("octave", -4.0f, 4.0f, 0.0f, "octaves", "Coarse pitch"),
          param("tune", -12.0f, 12.0f, 0.0f, "semitones", "Pitch offset"),
          param("fine", -100.0f, 100.0f, 0.0f, "cents", "Detune"),
          param("pulse_width", 0.05f, 0.95f, 0.5f, "", "Square wave pulse width"),
          param("fm_depth", 0.0f, 4.0f, 0.0f, "octaves", "FM depth per unit of fm input"),
          param("level", 0.0f, 1.0f, 0.8f, "", "Output level") },
        make<Vco>() });

    defs.push_back({ "noise", "White or pink noise.", Section::Both, {},
        { { "out", K::Audio, "" } },
        { choice("color", { "white", "pink" }, 0, "Noise color"),
          param("level", 0.0f, 1.0f, 0.5f, "", "Output level") },
        make<Noise>() });

    defs.push_back({ "vcf", "Resonant filter (low, high or band pass). cutoff input adds "
        "cv_depth octaves per unit (e.g. from an ADSR); pitch input with key_track makes the "
        "cutoff follow the note.",
        Section::Both,
        { { "in", K::Audio, "" }, { "cutoff", K::CV, "Cutoff modulation" },
          { "pitch", K::Pitch, "For key tracking" } },
        { { "out", K::Audio, "" } },
        { choice("mode", { "lowpass", "highpass", "bandpass" }, 0, "Filter type"),
          param("cutoff", 20.0f, 20000.0f, 1200.0f, "Hz", "Base cutoff frequency"),
          param("resonance", 0.0f, 1.0f, 0.2f, "", "Emphasis at the cutoff"),
          param("cv_depth", -8.0f, 8.0f, 4.0f, "octaves", "Cutoff change per unit of cutoff input"),
          param("key_track", 0.0f, 1.0f, 0.0f, "", "How much the cutoff follows pitch (1 = fully)") },
        make<Vcf>() });

    defs.push_back({ "vca", "Amplifier. Without a cv cable it passes the signal at 'gain'; "
        "with one (e.g. an ADSR) the cv shapes the volume.",
        Section::Both,
        { { "in", K::Audio, "" }, { "cv", K::CV, "Volume control, 0..1" } },
        { { "out", K::Audio, "" } },
        { param("gain", 0.0f, 1.0f, 1.0f, "", "Level") },
        make<Vca>() });

    defs.push_back({ "mixer", "Mixes four signals.", Section::Both,
        { { "in1", K::Audio, "" }, { "in2", K::Audio, "" }, { "in3", K::Audio, "" },
          { "in4", K::Audio, "" } },
        { { "out", K::Audio, "" } },
        { param("level1", 0.0f, 1.0f, 1.0f, "", ""), param("level2", 0.0f, 1.0f, 1.0f, "", ""),
          param("level3", 0.0f, 1.0f, 1.0f, "", ""), param("level4", 0.0f, 1.0f, 1.0f, "", ""),
          param("master", 0.0f, 1.0f, 0.5f, "", "Output level") },
        make<Mixer>() });

    defs.push_back({ "scale", "Attenuverter and offset: out = in * scale + offset. Use it to "
        "set modulation depth, invert a signal or shift a CV.",
        Section::Both, { { "in", K::CV, "" } }, { { "out", K::CV, "" } },
        { param("scale", -2.0f, 2.0f, 1.0f, "", "Multiplier (negative inverts)"),
          param("offset", -2.0f, 2.0f, 0.0f, "", "Added after scaling") },
        make<Scale>() });

    defs.push_back({ "slew", "Smooths changes (lag): separate rise and fall times.",
        Section::Both, { { "in", K::CV, "" } }, { { "out", K::CV, "" } },
        { param("rise", 0.0f, 5.0f, 0.05f, "s", "Time to rise"),
          param("fall", 0.0f, 5.0f, 0.05f, "s", "Time to fall") },
        make<Slew>() });

    defs.push_back({ "adsr", "Envelope: rises while the gate is high, falls when it ends. "
        "Outputs 0..1.",
        Section::Both, { { "gate", K::Gate, "" } }, { { "out", K::CV, "0..1" } },
        { param("attack", 0.0f, 10.0f, 0.01f, "s", ""), param("decay", 0.0f, 10.0f, 0.2f, "s", ""),
          param("sustain", 0.0f, 1.0f, 0.7f, "", "Level while held"),
          param("release", 0.0f, 10.0f, 0.3f, "s", "") },
        make<Adsr>() });

    defs.push_back({ "lfo", "Low-frequency oscillator for modulation. 'out' is -1..1, "
        "'unipolar' is 0..1. A rate cable adds 4 octaves of rate per unit; reset restarts "
        "the cycle on a gate.",
        Section::Both,
        { { "rate", K::CV, "Rate modulation" }, { "reset", K::Gate, "Restart on rising edge" } },
        { { "out", K::CV, "-1..1" }, { "unipolar", K::CV, "0..1" } },
        { choice("wave", { "sine", "triangle", "saw", "square", "random" }, 0, "Shape"),
          param("rate", 0.01f, 40.0f, 2.0f, "Hz", "Speed") },
        make<Lfo>() });

    addSequencingModules(defs);
    addRandomModules(defs);
    addFxModules(defs);
    return defs;
}

} // namespace

int ModuleDef::inputIndex(const std::string& name) const
{
    for (size_t i = 0; i < inputs.size(); ++i) if (inputs[i].name == name) return int(i);
    return -1;
}

int ModuleDef::outputIndex(const std::string& name) const
{
    for (size_t i = 0; i < outputs.size(); ++i) if (outputs[i].name == name) return int(i);
    return -1;
}

int ModuleDef::paramIndex(const std::string& name) const
{
    for (size_t i = 0; i < params.size(); ++i) if (params[i].name == name) return int(i);
    return -1;
}

const std::vector<ModuleDef>& allModules()
{
    static const std::vector<ModuleDef> defs = buildRegistry();
    return defs;
}

const ModuleDef* findModule(const std::string& type)
{
    for (const auto& d : allModules())
        if (d.type == type) return &d;
    return nullptr;
}

const char* portKindName(PortKind k)
{
    switch (k) {
    case PortKind::Audio: return "audio";
    case PortKind::CV:    return "cv";
    case PortKind::Gate:  return "gate";
    case PortKind::Pitch: return "pitch";
    }
    return "audio";
}

const char* sectionName(Section s)
{
    switch (s) {
    case Section::Voice:  return "voice";
    case Section::Global: return "global";
    case Section::Both:   return "both";
    }
    return "voice";
}

} // namespace OpenDaw::Modular
