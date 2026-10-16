#include "DrumEngine.h"
#include "modular/ModuleRegistry.h"
#include "modular/Patch.h"
#include <algorithm>
#include <cmath>

namespace OpenDaw::Drums {

namespace {
constexpr int kMaxPorts = 8;
constexpr float kSilence = 1.0e-5f;   // -100 dBFS
}

struct DrumEngine::PadVoice {
    int note = 0;
    int choke = 0;
    float gainL = 1.0f, gainR = 1.0f;

    // Choke fade: gain ramps to 0 after another pad in the group is hit.
    float chokeGain = 1.0f, chokeStep = 0.0f;
    // Samples of near-silence so far; at idleLimit the pad stops processing.
    int quietSamples = 0, idleLimit = 0;
    bool idle = true;

    // Synth pads: one modular module, fed a trigger pulse.
    std::unique_ptr<Modular::Module> module;
    std::vector<float> params;
    int numInputs = 0, numOutputs = 0;
    int triggerIn = -1, accentIn = -1, pitchIn = -1, outIndex = 0;
    float pitch = 0.0f;   // octaves, from the pad's tune
    float accent = 1.0f;
    int triggerLeft = 0, triggerLength = 1;
    bool retrigger = false;   // hold the trigger low one sample to make a new edge
    std::array<std::array<float, Modular::kSubBlock>, kMaxPorts> in{}, out{};
    std::array<bool, kMaxPorts> connected{};

    // Sample pads: two playheads, so a retrigger fades the old one out.
    std::shared_ptr<const SampleData> sample;
    double rate = 1.0;
    struct Playhead {
        double pos = 0.0;
        float gain = 0.0f, fade = 1.0f, fadeStep = 0.0f;
        bool active = false;
    };
    std::array<Playhead, 2> heads{};
    int current = 0;
    float retriggerStep = 0.0f;

    void startChoke(float step) { if (!idle) chokeStep = -step; }
};

DrumEngine::~DrumEngine() = default;

std::unique_ptr<DrumEngine> DrumEngine::build(const Kit& kit, double sampleRate,
                                              const SampleMap& samples, QStringList* warnings)
{
    std::unique_ptr<DrumEngine> engine(new DrumEngine());
    engine->sampleRate_ = sampleRate;
    const int idleLimit = int(kIdleSeconds * sampleRate);
    for (const auto& pad : kit.pads) {
        if (pad.note < 0 || pad.note > 127 || engine->byNote_[size_t(pad.note)]) continue;
        auto v = std::make_unique<PadVoice>();
        v->note = pad.note;
        v->choke = pad.choke;
        const float pan = std::clamp(pad.pan, -1.0f, 1.0f);
        v->gainL = pad.level * std::min(1.0f, 1.0f - pan);
        v->gainR = pad.level * std::min(1.0f, 1.0f + pan);
        v->idleLimit = idleLimit;

        if (pad.source == PadSource::Synth) {
            const auto* def = Modular::findModule(pad.module.toStdString());
            if (!def || def->inputIndex("trigger") < 0 || def->outputIndex("out") < 0
                || int(def->inputs.size()) > kMaxPorts || int(def->outputs.size()) > kMaxPorts) {
                if (warnings) *warnings << QString("Pad %1: '%2' can't be a synth pad; it is silent.")
                                               .arg(pad.note).arg(pad.module);
                continue;
            }
            v->module = def->create();
            v->module->prepare(sampleRate);
            v->numInputs = int(def->inputs.size());
            v->numOutputs = int(def->outputs.size());
            v->triggerIn = def->inputIndex("trigger");
            v->accentIn = def->inputIndex("accent");
            v->pitchIn = def->inputIndex("pitch");
            v->outIndex = def->outputIndex("out");
            v->pitch = pad.tune / 12.0f;
            v->triggerLength = std::max(1, int(std::lround(kTriggerSeconds * sampleRate)));
            for (int i : { v->triggerIn, v->accentIn, v->pitchIn })
                if (i >= 0) v->connected[size_t(i)] = true;
            for (size_t i = 0; i < def->params.size(); ++i) {
                const auto& pd = def->params[i];
                v->params.push_back(pad.params.value(QString::fromStdString(pd.name), pd.defaultValue));
            }
        } else {
            auto it = samples.find(pad.sample);
            if (it == samples.end() || !it->second || it->second->left.size() < 2) {
                if (warnings) *warnings << QString("Pad %1: the sample '%2' isn't loaded; it is silent.")
                                               .arg(pad.note).arg(pad.sample);
                continue;
            }
            v->sample = it->second;
            v->rate = std::exp2(pad.tune / 12.0) * v->sample->sampleRate / sampleRate;
            v->retriggerStep = float(1.0 / std::max(1.0, kRetriggerFadeSeconds * sampleRate));
        }
        engine->byNote_[size_t(pad.note)] = v.get();
        engine->pads_.push_back(std::move(v));
    }
    return engine;
}

void DrumEngine::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    auto* pad = byNote_[size_t(note)];
    if (!pad) return;
    velocity = std::clamp(velocity, 0.0f, 1.0f);

    if (pad->choke != 0) {
        const float step = float(1.0 / std::max(1.0, kChokeSeconds * sampleRate_));
        for (auto& other : pads_)
            if (other.get() != pad && other->choke == pad->choke) other->startChoke(step);
    }
    pad->chokeGain = 1.0f;
    pad->chokeStep = 0.0f;
    pad->idle = false;
    pad->quietSamples = 0;

    if (pad->module) {
        pad->accent = velocity;
        pad->retrigger = pad->triggerLeft > 0;
        pad->triggerLeft = pad->triggerLength;
    } else if (pad->sample) {
        auto& old = pad->heads[size_t(pad->current)];
        if (old.active) old.fadeStep = -pad->retriggerStep;
        pad->current ^= 1;
        auto& head = pad->heads[size_t(pad->current)];
        head = {};
        head.gain = velocity;
        head.active = true;
    }
}

void DrumEngine::allNotesOff()
{
    const float step = float(1.0 / std::max(1.0, kChokeSeconds * sampleRate_));
    for (auto& pad : pads_) pad->startChoke(step);
}

void DrumEngine::render(float* left, float* right, int numSamples)
{
    std::fill(left, left + numSamples, 0.0f);
    std::fill(right, right + numSamples, 0.0f);
    for (auto& pad : pads_)
        if (!pad->idle) renderPad(*pad, left, right, numSamples);
}

void DrumEngine::renderPad(PadVoice& pad, float* left, float* right, int numSamples)
{
    for (int start = 0; start < numSamples; start += Modular::kSubBlock) {
        const int n = std::min(Modular::kSubBlock, numSamples - start);
        float block[Modular::kSubBlock];

        if (pad.module) {
            for (int i = 0; i < n; ++i) {
                float trig = 0.0f;
                if (pad.retrigger) {
                    pad.retrigger = false;   // one low sample so the module sees a new edge
                } else if (pad.triggerLeft > 0) {
                    trig = 1.0f;
                    --pad.triggerLeft;
                }
                pad.in[size_t(pad.triggerIn)][size_t(i)] = trig;
                if (pad.accentIn >= 0) pad.in[size_t(pad.accentIn)][size_t(i)] = pad.accent;
                if (pad.pitchIn >= 0) pad.in[size_t(pad.pitchIn)][size_t(i)] = pad.pitch;
            }
            const float* ins[kMaxPorts];
            float* outs[kMaxPorts];
            for (int i = 0; i < pad.numInputs; ++i) ins[i] = pad.in[size_t(i)].data();
            for (int i = 0; i < pad.numOutputs; ++i) outs[i] = pad.out[size_t(i)].data();
            Modular::ProcessContext ctx;
            ctx.sampleRate = sampleRate_;
            ctx.numSamples = n;
            pad.module->process(ctx, ins, pad.connected.data(), outs, pad.params.data());
            for (int i = 0; i < n; ++i) {
                const float s = pad.out[size_t(pad.outIndex)][size_t(i)];
                block[i] = std::isfinite(s) ? s : 0.0f;
            }
        } else {
            const auto& data = *pad.sample;
            const bool stereo = !data.right.empty();
            const double last = double(data.left.size() - 1);
            std::fill(block, block + n, 0.0f);
            float blockR[Modular::kSubBlock] = {};
            for (auto& head : pad.heads) {
                if (!head.active) continue;
                for (int i = 0; i < n && head.active; ++i) {
                    if (head.pos >= last) { head.active = false; break; }
                    const size_t k = size_t(head.pos);
                    const float frac = float(head.pos - double(k));
                    const float g = head.gain * head.fade;
                    block[i] += g * (data.left[k] + frac * (data.left[k + 1] - data.left[k]));
                    if (stereo)
                        blockR[i] += g * (data.right[k] + frac * (data.right[k + 1] - data.right[k]));
                    head.pos += pad.rate;
                    if (head.fadeStep != 0.0f) {
                        head.fade += head.fadeStep;
                        if (head.fade <= 0.0f) head.active = false;
                    }
                }
            }
            if (stereo) {
                // Stereo samples keep their image; pan/level scale each side.
                for (int i = 0; i < n; ++i) {
                    const float g = pad.chokeGain;
                    left[start + i] += pad.gainL * g * block[i];
                    right[start + i] += pad.gainR * g * blockR[i];
                    if (pad.chokeStep != 0.0f) pad.chokeGain = std::max(0.0f, pad.chokeGain + pad.chokeStep);
                }
                const bool playing = pad.heads[0].active || pad.heads[1].active;
                if (!playing || pad.chokeGain <= 0.0f) pad.idle = true;
                continue;
            }
        }

        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float s = block[i] * pad.chokeGain;
            left[start + i] += pad.gainL * s;
            right[start + i] += pad.gainR * s;
            peak = std::max(peak, std::abs(s));
            if (pad.chokeStep != 0.0f) pad.chokeGain = std::max(0.0f, pad.chokeGain + pad.chokeStep);
        }

        if (pad.sample) {
            if (!(pad.heads[0].active || pad.heads[1].active) || pad.chokeGain <= 0.0f) pad.idle = true;
        } else if (pad.chokeGain <= 0.0f && pad.triggerLeft == 0) {
            pad.idle = true;   // choked: silent until the next hit
        } else if (peak < kSilence && pad.triggerLeft == 0) {
            pad.quietSamples += n;
            if (pad.quietSamples >= pad.idleLimit) pad.idle = true;
        } else {
            pad.quietSamples = 0;
        }
    }
}

} // namespace OpenDaw::Drums
