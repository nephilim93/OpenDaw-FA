#pragma once

#include "Module.h"
#include "Patch.h"
#include <array>
#include <memory>
#include <vector>

namespace OpenDaw::Modular {

// A patch turned into something the audio thread can run: modules in
// processing order with preallocated buffers, one copy of the voice section
// per voice. Built (and destroyed) on the message thread; render() and the
// note functions run on the audio thread and never allocate.
class CompiledPatch {
public:
    static constexpr int kNumVoices = 8;
    static constexpr int kMaxInputs = 32;  // per module

    static std::unique_ptr<CompiledPatch> build(const Patch& patch, double sampleRate);
    ~CompiledPatch();

    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();
    void setPitchBend(float semitones) { pitchBend_ = semitones; }
    void setModWheel(float value) { modWheel_ = value; }
    void setAftertouch(float value);

    // Adds the patch's output to left/right. macros: kNumMacros values, 0..1.
    void render(float* left, float* right, int numSamples, const float* macros,
                const Transport& transport = {});

    int numFeedbackCables() const { return numFeedback_; }

private:
    struct Source { bool global = false; int node = -1; int output = 0; };

    // A module in the patch, shared by all copies of its section.
    struct Node {
        const ModuleDef* def = nullptr;
        bool global = false;
        std::vector<std::vector<Source>> inputs;  // per input: cables into it
        std::vector<float> baseParams;
        std::vector<float> params;                // base + macros, per sub-block
    };

    // One running copy of a node (globals have one, voice nodes one per voice).
    struct Instance {
        std::unique_ptr<Module> module;
        std::vector<std::array<float, kSubBlock>> outputs;
        std::vector<std::array<float, kSubBlock>> sums;   // for multi-cable inputs
        std::vector<const float*> inputPtrs;
        std::vector<float*> outputPtrs;
        std::vector<char> connected;
        std::vector<std::pair<int, int>> sumInputs;        // (input index, sums index)
    };

    struct MacroBinding { int node; int param; float min, max; int macro; };

    struct Voice {
        VoiceState state;
        int note = -1;
        bool active = false;
        bool retrigger = false;
        bool gateOnNext = false;
        uint64_t startedAt = 0;
        int silentBlocks = 0;
        std::vector<Instance> instances;
        std::array<float, kSubBlock> outL{}, outR{};
    };

    CompiledPatch() = default;
    void wireInstance(Instance& inst, const Node& node, Voice* voice);
    const float* sourceBuffer(const Source& s, Voice* voice) const;
    void renderSubBlock(int n, const float* macros, float* outL, float* outR,
                        const Transport& transport);
    void processInstance(Instance& inst, const Node& node, Voice* voice,
                         const ProcessContext& ctx, int n);
    Voice* findVoiceFor(int note);
    void updateVoicePitch(Voice& v) const;

    double sampleRate_ = 44100.0;
    std::vector<Node> nodes_;
    std::vector<int> globalPre_, voiceOrder_, globalPost_;
    std::vector<Instance> globalInstances_;   // indexed by node (empty for voice nodes)
    std::array<Voice, kNumVoices> voices_;
    std::vector<MacroBinding> macroBindings_;
    std::vector<std::pair<int, int>> outputTaps_;  // (node, unused) for 'output' modules
    std::array<float, kSubBlock> zeros_{};
    std::array<float, kSubBlock> mixL_{}, mixR_{};
    float pitchBend_ = 0.0f, modWheel_ = 0.0f;
    uint64_t noteCounter_ = 0;
    int numFeedback_ = 0;
};

} // namespace OpenDaw::Modular
