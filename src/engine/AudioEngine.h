#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <memory>

namespace te = tracktion::engine;

namespace OpenDaw {

class AudioEngine : private juce::ChangeListener {
public:
    AudioEngine();
    ~AudioEngine();

    te::Engine&       engine()       { return *engine_; }
    const te::Engine& engine() const { return *engine_; }

    te::DeviceManager& deviceManager();

    void setDefaultAudioDevice();
    void restoreSavedAudioSettings();
    void saveAudioSettings();
    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;

    juce::StringArray getAvailableMidiInputDevices() const;
    juce::StringArray getAvailableMidiOutputDevices() const;
    // Enables MIDI devices according to the Settings window: inputs are on
    // unless switched off, outputs follow the saved list (untouched if the
    // user never chose).
    void applyMidiDeviceSettings();

private:
    // Re-applies MIDI settings when devices appear (e.g. a controller plugged in).
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    std::unique_ptr<te::Engine> engine_;
};

} // namespace OpenDaw
