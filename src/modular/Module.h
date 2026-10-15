#pragma once

#include <memory>

namespace OpenDaw::Modular {

// Audio is processed in fixed sub-blocks. A cable that closes a feedback
// loop is read one sub-block late, so feedback latency is kSubBlock samples.
constexpr int kSubBlock = 16;

// Per-voice note state, read by the Voice In module.
struct VoiceState {
    float pitch = 0.0f;       // 1 unit per octave, 0 = C4 (MIDI 60)
    float gate = 0.0f;        // 0 or 1
    float velocity = 0.0f;    // 0..1
    float aftertouch = 0.0f;  // 0..1
};

// The song's transport, for clocks and tempo-synced modules.
struct Transport {
    double bpm = 120.0;
    double beat = 0.0;     // song position in beats (quarter notes)
    bool playing = false;
    int beatsPerBar = 4;
};

// Values shared by every module for one sub-block.
struct ProcessContext {
    double sampleRate = 44100.0;
    int numSamples = kSubBlock;
    const VoiceState* voice = nullptr;  // null in the global section
    float modWheel = 0.0f;              // 0..1
    Transport transport;                // beat = position at this sub-block's start
    // Voice section: where Voice Out adds this voice's sound.
    float* voiceOut[2] = { nullptr, nullptr };
    // Global section: the summed voices, read by the Voices module.
    const float* voiceMix[2] = { nullptr, nullptr };
};

// One DSP module instance. Inputs that have no cable point at a buffer of
// zeros; `connected[i]` says whether input i has a cable. Parameters are
// passed in the order the module's definition lists them.
class Module {
public:
    virtual ~Module() = default;
    virtual void prepare(double /*sampleRate*/) {}
    virtual void reset() {}
    virtual void process(const ProcessContext& ctx,
                         const float* const* inputs, const bool* connected,
                         float* const* outputs, const float* params) = 0;
};

} // namespace OpenDaw::Modular
