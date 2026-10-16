#pragma once

#include "drums/DrumEngine.h"
#include "drums/DrumKit.h"
#include "engine/AudioEngine.h"
#include <memory>

namespace OpenDaw {

// OpenDaw's drum instrument: General MIDI drum notes play the pads of a kit
// (synth voices or samples). The kit lives as JSON in the plugin state, so it
// saves with the project and every change is undoable; each change is built
// on the message thread and swapped in for the audio thread.
class DrumMachinePlugin : public te::Plugin {
public:
    explicit DrumMachinePlugin(te::PluginCreationInfo info);
    ~DrumMachinePlugin() override;

    static const char* getPluginName() { return "Drum Machine"; }
    static const char* xmlTypeName;

    // Kit access (message thread).
    Drums::Kit kit() const { return kit_; }
    // Stores the kit as one undo step named undoName (empty: part of the
    // caller's current undo step).
    void setKit(const Drums::Kit& kit, const juce::String& undoName);
    // Why pads are silent (missing samples, ...), from the last build.
    QStringList warnings() const { return warnings_; }

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName(int) override { return "Drums"; }
    juce::String getSelectableDescription() override { return "Drum Machine"; }

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
    void loadKitFromState();
    void rebuild();
    std::shared_ptr<const Drums::SampleData> loadSample(const QString& path);

    juce::CachedValue<juce::String> kitJson_;
    Drums::Kit kit_;
    QStringList warnings_;
    double sampleRate_ = 44100.0;
    // Decoded samples by path, so a kit edit doesn't reload every file.
    Drums::SampleMap sampleCache_;

    juce::SpinLock engineLock_;
    std::unique_ptr<Drums::DrumEngine> engine_;
    static constexpr int kRenderChunk = 512;
    std::array<float, kRenderChunk> renderL_{}, renderR_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumMachinePlugin)
};

} // namespace OpenDaw
