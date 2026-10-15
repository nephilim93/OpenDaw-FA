#pragma once

#include "engine/AudioEngine.h"
#include "modular/CompiledPatch.h"
#include "modular/Patch.h"
#include <memory>
#include <optional>

namespace OpenDaw {

// OpenDaw's modular instrument. The patch lives as JSON in the plugin state,
// so it saves with the project and every change is undoable. Each change is
// compiled on the message thread and swapped in for the audio thread.
class ModularSynthPlugin : public te::Plugin {
public:
    explicit ModularSynthPlugin(te::PluginCreationInfo info);
    ~ModularSynthPlugin() override;

    static const char* getPluginName() { return "Modular Synth"; }
    static const char* xmlTypeName;

    // Patch access (message thread).
    Modular::Patch patch() const { return patch_; }
    te::AutomatableParameter* macro(int index) const
    {
        return index >= 0 && index < Modular::kNumMacros ? macros_[size_t(index)].get() : nullptr;
    }
    // Stores the patch as one undo step named undoName (empty: part of the
    // caller's current undo step).
    void setPatch(const Modular::Patch& patch, const juce::String& undoName);

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName(int) override { return "Modular"; }
    juce::String getSelectableDescription() override { return "Modular Synth"; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    double getLatencySeconds() override { return 0.0; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void getChannelNames(juce::StringArray* ins, juce::StringArray* outs) override;
    bool takesAudioInput() override { return false; }
    bool takesMidiInput() override { return true; }
    bool isSynth() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool canBeAddedToClip() override { return false; }

    void applyToBuffer(const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

protected:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;

private:
    void loadPatchFromState();
    void recompile();
    void handleMidi(Modular::CompiledPatch& cp, const juce::MidiMessage& m);

    juce::CachedValue<juce::String> patchJson_;
    std::array<juce::CachedValue<float>, Modular::kNumMacros> macroValues_;
    std::array<te::AutomatableParameter::Ptr, Modular::kNumMacros> macros_;

    Modular::Patch patch_;
    double sampleRate_ = 44100.0;
    // Song tempo and position for the audio thread (as Tracktion's LFO does).
    std::optional<tracktion::tempo::Sequence::Position> tempoPosition_;

    juce::SpinLock compiledLock_;
    std::unique_ptr<Modular::CompiledPatch> compiled_;
    static constexpr int kRenderChunk = 512;
    std::array<float, kRenderChunk> renderL_{}, renderR_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModularSynthPlugin)
};

} // namespace OpenDaw
