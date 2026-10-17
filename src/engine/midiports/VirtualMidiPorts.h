#pragma once

#include "engine/AudioEngine.h"
#include <QString>
#include <QStringList>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace OpenDaw {

// MIDI that leaves OpenDaw, written by the audio thread (one queue per
// MIDI Out plugin, single producer) and sent at its due time by the
// sender thread, so events keep their place inside a block.
class MidiSendQueue {
public:
    struct Event {
        std::array<juce::uint8, 3> bytes{};
        int size = 0;
        double dueMs = 0.0;   // juce::Time::getMillisecondCounterHiRes() clock
    };

    // Audio thread. Drops the event when the queue is full; messages longer
    // than 3 bytes (SysEx) aren't sent.
    bool push(const juce::MidiMessage& m, double dueMs);
    int pending() const { return fifo_.getNumReady(); }

private:
    friend class VirtualMidiPorts;
    static constexpr int kSize = 2048;
    juce::AbstractFifo fifo_{ kSize };
    std::array<Event, kSize> events_{};
    juce::String destination_;   // guarded by VirtualMidiPorts' lock
};

// OpenDaw's own MIDI ports, so other apps (VCV Rack, midimux, ...) can play
// OpenDaw and be played by it:
//  - "OpenDaw In": other apps send to it; tracks pick it as their MIDI input
//    (a Tracktion virtual input of the same name).
//  - "OpenDaw Out": tracks with the MIDI Out instrument send to it.
// macOS and Linux only: Windows has no virtual ports for apps (loopMIDI
// ports work there as ordinary devices). The sender also delivers MIDI Out
// tracks to any system MIDI output.
class VirtualMidiPorts : private juce::MidiInputCallback,
                         private juce::ChangeListener,
                         private juce::Thread {
public:
    static constexpr const char* kInName = "OpenDaw In";
    static constexpr const char* kOutName = "OpenDaw Out";
    // A MIDI Out destination: the virtual port, or a system output's name.
    static constexpr const char* kVirtualDestination = "OpenDaw Out";
    static constexpr const char* kEnabledKey = "midi/virtualPorts";

    explicit VirtualMidiPorts(te::Engine& engine);
    ~VirtualMidiPorts() override;

    // The engine's instance (plugins only know te::Engine); null before the
    // AudioEngine exists.
    static VirtualMidiPorts* instance();

    static bool supported();
    // The setting (on by default where supported).
    static bool enabledInSettings();

    // Publishes or withdraws both ports (message thread).
    void setEnabled(bool enabled);
    bool isEnabled() const { return virtualIn_ != nullptr; }

    // Devices that are OpenDaw's own ports seen from the outside (our "Out"
    // is a source and "In" a destination to every app, including this one):
    // hidden from device lists and kept disabled, so MIDI never loops back.
    static bool isOwnPortSeenAsDevice(const juce::String& deviceName, bool isInput);

    // Where MIDI Out tracks can send: the virtual port (when published), then
    // the system's outputs.
    QStringList destinations() const;

    // MIDI Out plugins (message thread).
    void addQueue(MidiSendQueue* queue, const juce::String& destination);
    void setDestination(MidiSendQueue* queue, const juce::String& destination);
    void removeQueue(MidiSendQueue* queue);
    // Why the destination can't be reached right now (empty if it can).
    juce::String destinationProblem(const juce::String& destination) const;

    // Messages received on "OpenDaw In", and those handed on to Tracktion
    // (for tests and diagnostics).
    int receivedCount() const { return received_.load(); }
    int forwardedCount() const { return forwarded_.load(); }

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void run() override;

    void findTracktionInput();
    juce::MidiOutput* outputFor(const juce::String& destination);
    void openOutput(const juce::String& destination);
    void closeUnusedOutputs();

    te::Engine& engine_;
    std::unique_ptr<juce::MidiInput> virtualIn_;
    std::shared_ptr<juce::MidiOutput> virtualOut_;

    juce::SpinLock inputLock_;
    std::shared_ptr<te::MidiInputDevice> tracktionIn_;
    tracktion::MPESourceID sourceId_ = tracktion::createUniqueMPESourceID();
    std::atomic<int> received_{ 0 }, forwarded_{ 0 };

    juce::CriticalSection lock_;   // queues_ and outputs_; never taken by the audio thread
    std::vector<MidiSendQueue*> queues_;
    std::map<juce::String, std::shared_ptr<juce::MidiOutput>> outputs_;
};

} // namespace OpenDaw
