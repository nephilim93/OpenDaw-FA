#pragma once

#include "engine/AudioEngine.h"

namespace OpenDaw {

// MIDI FX that reshapes note-on velocities before they reach the instrument:
// scale, offset, randomize, then limit to a range.
class VelocityFxPlugin : public te::Plugin {
public:
    explicit VelocityFxPlugin(te::PluginCreationInfo info);
    ~VelocityFxPlugin() override;

    static const char* getPluginName() { return "Velocity"; }
    static const char* xmlTypeName;

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName(int) override { return "Vel"; }
    juce::String getSelectableDescription() override { return "Velocity MIDI FX"; }

    void initialise(const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    double getLatencySeconds() override { return 0.0; }
    int getNumOutputChannelsGivenInputs(int) override { return 0; }
    void getChannelNames(juce::StringArray*, juce::StringArray*) override {}
    bool takesAudioInput() override { return false; }
    bool canBeAddedToClip() override { return false; }

    void applyToBuffer(const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    // Applies the current settings to one velocity (1..127).
    int transform(int velocity);

private:
    juce::CachedValue<float> scaleValue, offsetValue, randomValue, minValue, maxValue;
    te::AutomatableParameter::Ptr scale, offset, random, minimum, maximum;
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VelocityFxPlugin)
};

} // namespace OpenDaw
