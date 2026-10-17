#pragma once

#include "engine/AudioEngine.h"
#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QCheckBox>
#include <functional>

namespace OpenDaw {

// Audio page of the Settings window. Changes apply immediately and are
// saved so they are restored on the next launch.
class AudioSettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit AudioSettingsPage(AudioEngine& engine, QWidget* parent = nullptr);

    // Called after the device changes, e.g. to re-enable track inputs.
    void setDeviceChangedCallback(std::function<void()> cb) { onDeviceChanged_ = std::move(cb); }

private:
    void populateDeviceTypes();
    void populateDevices();
    void populateSampleRates();
    void populateBufferSizes();
    void applySetup(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup&)>& change);
    void updateDriverNote();
    void updateLatency();

    AudioEngine& engine_;
    std::function<void()> onDeviceChanged_;

    QComboBox* deviceTypeCombo_  = nullptr;
    QComboBox* inputCombo_       = nullptr;
    QComboBox* outputCombo_      = nullptr;
    QComboBox* sampleRateCombo_  = nullptr;
    QComboBox* bufferSizeCombo_  = nullptr;
    QLabel*    latencyLabel_     = nullptr;
    QCheckBox* allChannels_      = nullptr;
    QLabel*    driverNote_       = nullptr;
    QLabel*    errorLabel_       = nullptr;
};

} // namespace OpenDaw
