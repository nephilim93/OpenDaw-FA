#pragma once

#include "VirtualMidiPorts.h"

namespace OpenDaw {

// An instrument that plays nothing itself: the track's MIDI (clips and live
// input) goes to another app or device, through OpenDaw's virtual "OpenDaw
// Out" port or any system MIDI output (a loopMIDI port on Windows, a
// hardware synth). Events keep their timing inside the audio block.
class MidiOutPlugin : public te::Plugin {
public:
    explicit MidiOutPlugin(te::PluginCreationInfo info);
    ~MidiOutPlugin() override;

    static const char* getPluginName() { return "MIDI Out"; }
    static const char* xmlTypeName;

    // A destination name (VirtualMidiPorts::destinations()).
    juce::String destination() const { return destination_.get(); }
    // 0 keeps each message's channel; 1..16 sends everything on that one.
    int channel() const { return channel_.get(); }
    // One undo step each, named undoName (empty: part of the current one).
    void setDestination(const juce::String& destination, const juce::String& undoName);
    void setChannel(int channel, const juce::String& undoName);
    // Why nothing arrives (empty when the destination is reachable).
    juce::String problem() const;

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName(int) override { return "MIDI Out"; }
    juce::String getSelectableDescription() override { return "MIDI Out"; }

    void initialise(const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    double getLatencySeconds() override { return 0.0; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    bool takesAudioInput() override { return false; }
    bool takesMidiInput() override { return true; }
    bool isSynth() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return false; }
    bool canBeAddedToClip() override { return false; }

    void applyToBuffer(const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    // For tests: events waiting for the sender thread.
    int pendingEvents() const { return queue_.pending(); }

protected:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;

private:
    void push(const juce::MidiMessage& m, double dueMs);

    juce::CachedValue<juce::String> destination_;
    juce::CachedValue<int> channel_;
    std::atomic<int> sendChannel_{ 0 };
    MidiSendQueue queue_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiOutPlugin)
};

} // namespace OpenDaw
