#pragma once

#include "DrumKit.h"
#include "modular/Module.h"
#include <QStringList>
#include <array>
#include <map>
#include <memory>
#include <vector>

namespace OpenDaw::Drums {

// A decoded audio file for a sample pad. `right` empty means mono.
struct SampleData {
    std::vector<float> left, right;
    double sampleRate = 44100.0;
};
using SampleMap = std::map<QString, std::shared_ptr<const SampleData>>;

// Plays a kit. Built on the message thread from a Kit and its decoded
// samples; noteOn/render run on the audio thread and never allocate.
class DrumEngine {
public:
    // Pads whose sample is missing from `samples` stay silent (a warning says so).
    static std::unique_ptr<DrumEngine> build(const Kit& kit, double sampleRate,
                                             const SampleMap& samples, QStringList* warnings = nullptr);
    ~DrumEngine();

    // One-shot: drums ignore note-off. Notes without a pad are ignored.
    void noteOn(int note, float velocity);
    void allNotesOff();   // fades every pad out
    // Writes (not adds) numSamples of the kit's output.
    void render(float* left, float* right, int numSamples);

    // Seconds a pad's output may stay below the silence threshold before it
    // stops being processed until its next hit.
    static constexpr double kIdleSeconds = 1.0;
    static constexpr double kChokeSeconds = 0.005;   // fade when choked
    static constexpr double kRetriggerFadeSeconds = 0.003;   // sample pads
    static constexpr double kTriggerSeconds = 0.001;   // synth trigger pulse

private:
    struct PadVoice;
    DrumEngine() = default;
    void renderPad(PadVoice& pad, float* left, float* right, int numSamples);

    double sampleRate_ = 44100.0;
    std::vector<std::unique_ptr<PadVoice>> pads_;
    std::array<PadVoice*, 128> byNote_{};
};

} // namespace OpenDaw::Drums
