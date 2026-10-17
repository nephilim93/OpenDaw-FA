#pragma once

#include "VirtualMidiPorts.h"
#include <memory>

namespace OpenDaw {

// Sends MIDI clock (24 per beat) with song position and start / continue /
// stop on "OpenDaw Out", so other apps (VCV Rack, ...) follow OpenDaw's
// tempo and transport. Lives hidden at the start of the master plugin list
// (EditManager adds it), where it sees every block's edit time; the audio
// passes through untouched. Ticks are timed like MIDI Out notes, so the two
// stay together.
class MidiClockPlugin : public te::Plugin {
public:
    explicit MidiClockPlugin(te::PluginCreationInfo info);
    ~MidiClockPlugin() override;

    static const char* getPluginName() { return "MIDI Clock"; }
    static const char* xmlTypeName;

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getShortName(int) override { return "Clock"; }
    juce::String getSelectableDescription() override { return "MIDI Clock"; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    double getLatencySeconds() override { return 0.0; }
    int getNumOutputChannelsGivenInputs(int numInputs) override { return numInputs; }
    bool takesAudioInput() override { return true; }
    bool takesMidiInput() override { return false; }
    bool isSynth() override { return false; }
    bool producesAudioWhenNoAudioInput() override { return false; }
    bool canBeAddedToClip() override { return false; }
    bool canBeAddedToRack() override { return false; }
    bool canBeMoved() override { return false; }

    void applyToBuffer(const te::PluginRenderContext&) override;

    // For tests: events waiting for the sender thread.
    int pendingEvents() const { return queue_.pending(); }

private:
    void push(const juce::MidiMessage& m, double dueMs) { queue_.push(m, dueMs); }

    MidiSendQueue queue_;
    juce::SpinLock positionLock_;
    std::unique_ptr<tracktion::tempo::Sequence::Position> position_;
    bool wasPlaying_ = false;
    bool needsPosition_ = true;
    double lastBlockStart_ = -1.0e9;
    double lastBlockEndPpq_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiClockPlugin)
};

} // namespace OpenDaw
