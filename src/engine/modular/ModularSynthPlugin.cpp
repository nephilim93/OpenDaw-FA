#include "ModularSynthPlugin.h"
#include <QJsonDocument>
#include <cmath>

namespace OpenDaw {

namespace {
const juce::Identifier kPatchId("modularPatch");

QJsonObject parseJson(const juce::String& text)
{
    return QJsonDocument::fromJson(QByteArray(text.toRawUTF8())).object();
}

juce::String toJsonString(const Modular::Patch& patch)
{
    const auto bytes = QJsonDocument(patch.toJson()).toJson(QJsonDocument::Compact);
    return juce::String::fromUTF8(bytes.constData(), int(bytes.size()));
}
}

const char* ModularSynthPlugin::xmlTypeName = "openDawModularSynth";

ModularSynthPlugin::ModularSynthPlugin(te::PluginCreationInfo info)
    : te::Plugin(info)
{
    auto* um = getUndoManager();
    patchJson_.referTo(state, kPatchId, um);

    for (int i = 0; i < Modular::kNumMacros; ++i) {
        const juce::String id = "macro" + juce::String(i + 1);
        macros_[size_t(i)] = addParam(id, "Macro " + juce::String(i + 1), { 0.0f, 1.0f });
        macroValues_[size_t(i)].referTo(state, juce::Identifier(id), um, 0.0f);
        macros_[size_t(i)]->attachToCurrentValue(macroValues_[size_t(i)]);
    }

    // A new instrument starts from the init patch (not an undo step).
    if (patchJson_.get().isEmpty())
        patchJson_.setValue(toJsonString(Modular::Patch::initPatch()), nullptr);
    loadPatchFromState();
}

ModularSynthPlugin::~ModularSynthPlugin()
{
    notifyListenersOfDeletion();
    for (auto& m : macros_) m->detachFromCurrentValue();

    const juce::SpinLock::ScopedLockType lock(compiledLock_);
    compiled_.reset();
}

void ModularSynthPlugin::setPatch(const Modular::Patch& patch, const juce::String& undoName)
{
    if (auto* um = getUndoManager())
        um->beginNewTransaction(undoName);
    patchJson_ = toJsonString(patch);  // valueTreePropertyChanged recompiles
}

void ModularSynthPlugin::loadPatchFromState()
{
    QString error;
    auto parsed = Modular::Patch::fromJson(parseJson(patchJson_.get()), &error);
    patch_ = parsed ? *parsed : Modular::Patch::initPatch();
    recompile();
}

void ModularSynthPlugin::recompile()
{
    auto fresh = Modular::CompiledPatch::build(patch_, sampleRate_);
    {
        const juce::SpinLock::ScopedLockType lock(compiledLock_);
        std::swap(compiled_, fresh);
    }
    // `fresh` now holds the old patch and is freed here, off the audio thread.
}

void ModularSynthPlugin::initialise(const te::PluginInitialisationInfo& info)
{
    tempoPosition_.emplace(te::createPosition(edit.tempoSequence));

    if (info.sampleRate > 0.0 && std::abs(info.sampleRate - sampleRate_) > 0.5) {
        sampleRate_ = info.sampleRate;
        recompile();
    }
}

void ModularSynthPlugin::getChannelNames(juce::StringArray* ins, juce::StringArray* outs)
{
    if (ins) ins->clear();
    if (outs) { outs->clear(); outs->add("Left"); outs->add("Right"); }
}

void ModularSynthPlugin::valueTreePropertyChanged(juce::ValueTree& v, const juce::Identifier& id)
{
    te::Plugin::valueTreePropertyChanged(v, id);
    if (v == state && id == kPatchId)
        loadPatchFromState();
}

void ModularSynthPlugin::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    if (v.hasProperty(kPatchId))
        patchJson_.setValue(v.getProperty(kPatchId).toString(), nullptr);
    for (int i = 0; i < Modular::kNumMacros; ++i) {
        const juce::Identifier id("macro" + juce::String(i + 1));
        if (v.hasProperty(id))
            macroValues_[size_t(i)].setValue(float(v.getProperty(id)), nullptr);
    }
    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
    loadPatchFromState();
}

void ModularSynthPlugin::handleMidi(Modular::CompiledPatch& cp, const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        cp.noteOn(m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        cp.noteOff(m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        cp.allNotesOff();
    else if (m.isPitchWheel())
        cp.setPitchBend(float(m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f);
    else if (m.isControllerOfType(1))
        cp.setModWheel(float(m.getControllerValue()) / 127.0f);
    else if (m.isChannelPressure())
        cp.setAftertouch(float(m.getChannelPressureValue()) / 127.0f);
}

void ModularSynthPlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    auto* dest = fc.destBuffer;
    const int numSamples = fc.bufferNumSamples;
    if (dest)
        for (int ch = 0; ch < dest->getNumChannels(); ++ch)
            dest->clear(ch, fc.bufferStartSample, numSamples);

    const juce::SpinLock::ScopedTryLockType lock(compiledLock_);
    if (!lock.isLocked() || !compiled_) return;  // a patch swap is in progress
    auto& cp = *compiled_;

    Modular::Transport transport;
    transport.playing = fc.isPlaying;
    if (tempoPosition_) {
        tempoPosition_->set(fc.editTime.getStart());
        transport.bpm = tempoPosition_->getTempo();
        transport.beat = tempoPosition_->getBeats().inBeats();
        transport.beatsPerBar = tempoPosition_->getTimeSignature().numerator;
    }
    const double beatsPerSample = transport.bpm / 60.0 / sampleRate_;

    float macroValues[Modular::kNumMacros];
    for (int i = 0; i < Modular::kNumMacros; ++i)
        macroValues[i] = macros_[size_t(i)]->getCurrentValue();

    // Render between MIDI events so notes start at the right sample (to
    // within one 16-sample sub-block).
    auto renderRange = [&](int from, int to) {
        while (from < to) {
            const int n = std::min(kRenderChunk, to - from);
            std::fill(renderL_.begin(), renderL_.begin() + n, 0.0f);
            std::fill(renderR_.begin(), renderR_.begin() + n, 0.0f);
            Modular::Transport t = transport;
            t.beat += from * beatsPerSample;
            cp.render(renderL_.data(), renderR_.data(), n, macroValues, t);
            if (dest) {
                // Gentle limiter: transparent at normal levels, never above 1.
                for (int i = 0; i < n; ++i) {
                    const float l = std::tanh(renderL_[size_t(i)]);
                    const float r = std::tanh(renderR_[size_t(i)]);
                    if (dest->getNumChannels() > 0)
                        dest->setSample(0, fc.bufferStartSample + from + i, l);
                    if (dest->getNumChannels() > 1)
                        dest->setSample(1, fc.bufferStartSample + from + i, r);
                }
            }
            from += n;
        }
    };

    int position = 0;
    if (fc.bufferForMidiMessages) {
        fc.bufferForMidiMessages->sortByTimestamp();
        for (auto& m : *fc.bufferForMidiMessages) {
            const int at = juce::jlimit(0, numSamples,
                                        juce::roundToInt(m.getTimeStamp() * sampleRate_));
            renderRange(position, at);
            position = std::max(position, at);
            handleMidi(cp, m);
        }
    }
    renderRange(position, numSamples);
}

} // namespace OpenDaw
