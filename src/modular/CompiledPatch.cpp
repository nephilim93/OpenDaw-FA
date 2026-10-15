#include "CompiledPatch.h"
#include <QHash>
#include <functional>
#include <algorithm>
#include <cmath>

namespace OpenDaw::Modular {

CompiledPatch::~CompiledPatch() = default;

std::unique_ptr<CompiledPatch> CompiledPatch::build(const Patch& patch, double sampleRate)
{
    std::unique_ptr<CompiledPatch> cp(new CompiledPatch());
    cp->sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Nodes, one per patch module.
    QHash<QString, int> indexOf;
    for (const auto& m : patch.modules) {
        Node node;
        node.def = findModule(m.type.toStdString());
        if (!node.def) return nullptr;
        node.global = m.section == Section::Global;
        node.inputs.resize(node.def->inputs.size());
        for (int p = 0; p < int(node.def->params.size()); ++p)
            node.baseParams.push_back(patch.paramValue(m, p));
        node.params = node.baseParams;
        indexOf.insert(m.id, int(cp->nodes_.size()));
        cp->nodes_.push_back(std::move(node));
    }

    for (const auto& c : patch.cables) {
        const int src = indexOf.value(c.from.module, -1);
        const int dst = indexOf.value(c.to.module, -1);
        if (src < 0 || dst < 0) continue;
        const int out = cp->nodes_[size_t(src)].def->outputIndex(c.from.port.toStdString());
        const int in = cp->nodes_[size_t(dst)].def->inputIndex(c.to.port.toStdString());
        if (out < 0 || in < 0) continue;
        cp->nodes_[size_t(dst)].inputs[size_t(in)].push_back(
            { cp->nodes_[size_t(src)].global, src, out });
    }

    // Processing order per section by depth-first search over same-section
    // cables. A cable back to a module still being visited closes a loop:
    // its source runs later, so the input reads the previous sub-block.
    const int n = int(cp->nodes_.size());
    std::vector<int> state(size_t(n), 0);  // 0 new, 1 visiting, 2 done
    std::vector<int> order;
    std::function<void(int)> visit = [&](int i) {
        state[size_t(i)] = 1;
        for (const auto& sources : cp->nodes_[size_t(i)].inputs)
            for (const auto& s : sources) {
                if (s.global != cp->nodes_[size_t(i)].global) continue;
                if (state[size_t(s.node)] == 1) { ++cp->numFeedback_; continue; }
                if (state[size_t(s.node)] == 0) visit(s.node);
            }
        state[size_t(i)] = 2;
        order.push_back(i);
    };
    for (int i = 0; i < n; ++i)
        if (state[size_t(i)] == 0) visit(i);

    // Globals downstream of the summed voices run after the voices.
    std::vector<char> afterVoices(size_t(n), 0);
    for (int i : order) {
        const auto& node = cp->nodes_[size_t(i)];
        if (!node.global) { cp->voiceOrder_.push_back(i); continue; }
        bool after = node.def->type == "voices";
        for (const auto& sources : node.inputs)
            for (const auto& s : sources)
                if (s.global && afterVoices[size_t(s.node)]) after = true;
        afterVoices[size_t(i)] = after;
        (after ? cp->globalPost_ : cp->globalPre_).push_back(i);
        if (node.def->type == "output") cp->outputTaps_.push_back({ i, 0 });
    }

    // Instances, then wiring once every buffer exists (buffers never move).
    auto makeInstance = [&](const Node& node) {
        Instance inst;
        inst.module = node.def->create();
        inst.module->prepare(cp->sampleRate_);
        inst.module->reset();
        inst.outputs.resize(node.def->outputs.size());
        for (auto& o : inst.outputs) o.fill(0.0f);
        return inst;
    };
    cp->globalInstances_.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        if (cp->nodes_[size_t(i)].global)
            cp->globalInstances_[size_t(i)] = makeInstance(cp->nodes_[size_t(i)]);
    for (auto& v : cp->voices_) {
        v.instances.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            if (!cp->nodes_[size_t(i)].global)
                v.instances[size_t(i)] = makeInstance(cp->nodes_[size_t(i)]);
    }
    for (int i = 0; i < n; ++i) {
        const auto& node = cp->nodes_[size_t(i)];
        if (node.global)
            cp->wireInstance(cp->globalInstances_[size_t(i)], node, nullptr);
        else
            for (auto& v : cp->voices_) cp->wireInstance(v.instances[size_t(i)], node, &v);
    }

    for (int m = 0; m < kNumMacros; ++m)
        for (const auto& t : patch.macros[size_t(m)].targets) {
            const int node = indexOf.value(t.module, -1);
            if (node < 0) continue;
            const int param = cp->nodes_[size_t(node)].def->paramIndex(t.param.toStdString());
            if (param >= 0) cp->macroBindings_.push_back({ node, param, t.min, t.max, m });
        }

    return cp;
}

const float* CompiledPatch::sourceBuffer(const Source& s, Voice* voice) const
{
    const auto& inst = s.global ? globalInstances_[size_t(s.node)] : voice->instances[size_t(s.node)];
    return inst.outputs[size_t(s.output)].data();
}

void CompiledPatch::wireInstance(Instance& inst, const Node& node, Voice* voice)
{
    const size_t numIn = node.inputs.size();
    inst.inputPtrs.assign(numIn, zeros_.data());
    inst.connected.assign(numIn, 0);
    inst.outputPtrs.clear();
    for (auto& o : inst.outputs) inst.outputPtrs.push_back(o.data());

    // Inputs with several cables are summed into their own buffer.
    int multi = 0;
    for (const auto& sources : node.inputs) if (sources.size() > 1) ++multi;
    inst.sums.resize(size_t(multi));

    int sumIndex = 0;
    for (size_t in = 0; in < numIn; ++in) {
        const auto& sources = node.inputs[in];
        if (sources.empty()) continue;
        inst.connected[in] = 1;
        if (sources.size() == 1) {
            inst.inputPtrs[in] = sourceBuffer(sources[0], voice);
        } else {
            inst.inputPtrs[in] = inst.sums[size_t(sumIndex)].data();
            inst.sumInputs.push_back({ int(in), sumIndex });
            ++sumIndex;
        }
    }
}

void CompiledPatch::processInstance(Instance& inst, const Node& node, Voice* voice,
                                    const ProcessContext& ctx, int n)
{
    for (const auto& [in, sumIndex] : inst.sumInputs) {
        auto& sum = inst.sums[size_t(sumIndex)];
        std::fill(sum.begin(), sum.begin() + n, 0.0f);
        for (const auto& s : node.inputs[size_t(in)]) {
            const float* src = sourceBuffer(s, voice);
            for (int i = 0; i < n; ++i) sum[size_t(i)] += src[i];
        }
    }

    bool connected[kMaxInputs] = {};
    for (size_t i = 0; i < inst.connected.size() && i < size_t(kMaxInputs); ++i)
        connected[i] = inst.connected[i] != 0;
    inst.module->process(ctx, inst.inputPtrs.data(), connected, inst.outputPtrs.data(),
                         node.params.data());
}

void CompiledPatch::renderSubBlock(int n, const float* macros, float* outL, float* outR,
                                   const Transport& transport)
{
    // Parameters: patch values, overridden by macros where mapped.
    for (auto& node : nodes_)
        std::copy(node.baseParams.begin(), node.baseParams.end(), node.params.begin());
    for (const auto& b : macroBindings_) {
        const float m = std::clamp(macros ? macros[b.macro] : 0.0f, 0.0f, 1.0f);
        nodes_[size_t(b.node)].params[size_t(b.param)] = b.min + (b.max - b.min) * m;
    }

    ProcessContext gctx;
    gctx.sampleRate = sampleRate_;
    gctx.numSamples = n;
    gctx.modWheel = modWheel_;
    gctx.transport = transport;
    gctx.voiceMix[0] = mixL_.data();
    gctx.voiceMix[1] = mixR_.data();

    for (int i : globalPre_)
        processInstance(globalInstances_[size_t(i)], nodes_[size_t(i)], nullptr, gctx, n);

    std::fill(mixL_.begin(), mixL_.begin() + n, 0.0f);
    std::fill(mixR_.begin(), mixR_.begin() + n, 0.0f);

    for (auto& v : voices_) {
        if (!v.active) continue;

        // A re-struck voice drops its gate for one sub-block so envelopes restart.
        if (v.retrigger) { v.state.gate = 0.0f; v.retrigger = false; v.gateOnNext = true; }
        else if (v.gateOnNext) { v.state.gate = 1.0f; v.gateOnNext = false; }
        updateVoicePitch(v);

        std::fill(v.outL.begin(), v.outL.begin() + n, 0.0f);
        std::fill(v.outR.begin(), v.outR.begin() + n, 0.0f);
        ProcessContext vctx = gctx;
        vctx.voice = &v.state;
        vctx.voiceOut[0] = v.outL.data();
        vctx.voiceOut[1] = v.outR.data();
        for (int i : voiceOrder_)
            processInstance(v.instances[size_t(i)], nodes_[size_t(i)], &v, vctx, n);

        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            mixL_[size_t(i)] += v.outL[size_t(i)];
            mixR_[size_t(i)] += v.outR[size_t(i)];
            peak = std::max(peak, std::max(std::abs(v.outL[size_t(i)]), std::abs(v.outR[size_t(i)])));
        }

        // A released voice that has gone quiet stops using CPU.
        if (v.state.gate < 0.5f && !v.gateOnNext && peak < 1e-4f) {
            if (++v.silentBlocks * kSubBlock > int(sampleRate_ * 0.3)) v.active = false;
        } else {
            v.silentBlocks = 0;
        }
    }

    for (int i : globalPost_)
        processInstance(globalInstances_[size_t(i)], nodes_[size_t(i)], nullptr, gctx, n);

    // Output modules: left (and right, or left again) times volume.
    for (const auto& [node, unused] : outputTaps_) {
        (void)unused;
        const auto& inst = globalInstances_[size_t(node)];
        const float volume = nodes_[size_t(node)].params[0];
        const float* l = inst.inputPtrs[0];
        const float* r = inst.connected[1] ? inst.inputPtrs[1] : inst.inputPtrs[0];
        for (int i = 0; i < n; ++i) {
            outL[i] += l[i] * volume;
            outR[i] += r[i] * volume;
        }
    }
}

void CompiledPatch::render(float* left, float* right, int numSamples, const float* macros,
                           const Transport& transport)
{
    const double beatsPerSample = transport.bpm / 60.0 / sampleRate_;
    for (int start = 0; start < numSamples; start += kSubBlock) {
        const int n = std::min(kSubBlock, numSamples - start);
        Transport t = transport;
        t.beat += start * beatsPerSample;
        renderSubBlock(n, macros, left + start, right + start, t);
    }
}

// ── Voices ─────────────────────────────────────────────────────────────────

void CompiledPatch::updateVoicePitch(Voice& v) const
{
    if (v.note >= 0)
        v.state.pitch = (float(v.note - 60) + pitchBend_) / 12.0f;
}

CompiledPatch::Voice* CompiledPatch::findVoiceFor(int note)
{
    for (auto& v : voices_) if (v.active && v.note == note) return &v;   // re-strike
    for (auto& v : voices_) if (!v.active) return &v;                    // free
    // Steal: prefer released voices, then the oldest.
    Voice* best = nullptr;
    for (auto& v : voices_) {
        const bool released = v.state.gate < 0.5f;
        if (!best) { best = &v; continue; }
        const bool bestReleased = best->state.gate < 0.5f;
        if (released != bestReleased) { if (released) best = &v; continue; }
        if (v.startedAt < best->startedAt) best = &v;
    }
    return best;
}

void CompiledPatch::noteOn(int note, float velocity)
{
    Voice* v = findVoiceFor(note);
    if (!v) return;
    if (!v->active) {
        for (auto& inst : v->instances) if (inst.module) inst.module->reset();
        v->active = true;
        v->state.gate = 1.0f;
        v->retrigger = false;
        v->gateOnNext = false;
    } else {
        v->retrigger = true;
    }
    v->note = note;
    v->state.velocity = std::clamp(velocity, 0.0f, 1.0f);
    v->startedAt = ++noteCounter_;
    v->silentBlocks = 0;
    updateVoicePitch(*v);
}

void CompiledPatch::noteOff(int note)
{
    for (auto& v : voices_)
        if (v.active && v.note == note) {
            v.state.gate = 0.0f;
            v.gateOnNext = false;
            v.retrigger = false;
            v.note = -1 - note;  // keep the pitch (see updateVoicePitch) but free the note
        }
}

void CompiledPatch::allNotesOff()
{
    for (auto& v : voices_) {
        v.state.gate = 0.0f;
        v.gateOnNext = false;
        v.retrigger = false;
    }
}

void CompiledPatch::setAftertouch(float value)
{
    for (auto& v : voices_) v.state.aftertouch = std::clamp(value, 0.0f, 1.0f);
}

} // namespace OpenDaw::Modular
