#include "AudioEngine.h"
#include "midifx/VelocityFxPlugin.h"
#include <QSettings>

namespace OpenDaw {

#if JUCE_LINUX
namespace {
// Tracktion opens the default device with every channel it reports before
// setDefaultAudioDevice() narrows it to stereo. Through PipeWire's ALSA
// plugin that is 128 capture channels, which crashes PipeWire's converter.
// Skip the automatic open; setDefaultAudioDevice() opens the device instead.
//
// PipeWire's "default" device also reports 128 channel names while only the
// opened ones reach the audio callback, so describe wave devices for the
// active channels only. Otherwise Tracktion creates inputs for channels that
// never arrive and asserts on every audio block.
class OpenDawEngineBehaviour : public te::EngineBehaviour {
public:
    bool autoInitialiseDeviceManager() override { return false; }

    bool isDescriptionOfWaveDevicesSupported() override { return true; }

    void describeWaveDevices(std::vector<te::WaveDeviceDescription>& descriptions,
                             juce::AudioIODevice& device, bool isInput) override
    {
        auto names = isInput ? device.getInputChannelNames() : device.getOutputChannelNames();
        auto active = isInput ? device.getActiveInputChannels() : device.getActiveOutputChannels();
        int numChannels = juce::jmin(active.countNumberOfSetBits(), names.size());

        juce::String prefix = isInput ? "Input " : "Output ";
        for (int i = 0; i < numChannels; i += 2) {
            if (i + 1 < numChannels)
                descriptions.emplace_back(prefix + juce::String(i + 1) + " + " + juce::String(i + 2),
                                          i, i + 1, true);
            else
                descriptions.emplace_back(prefix + juce::String(i + 1), i, -1, true);
        }
    }
};
}
#endif

AudioEngine::AudioEngine()
{
#if JUCE_LINUX
    engine_ = std::make_unique<te::Engine>("OpenDaw", nullptr,
                                           std::make_unique<OpenDawEngineBehaviour>());
#else
    engine_ = std::make_unique<te::Engine>("OpenDaw");
#endif

    // OpenDaw's own plugin types, so edits can create and reload them.
    engine_->getPluginManager().createBuiltInType<VelocityFxPlugin>();

    engine_->getDeviceManager().addChangeListener(this);
}

AudioEngine::~AudioEngine()
{
    engine_->getDeviceManager().removeChangeListener(this);
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    applyMidiDeviceSettings();
}

te::DeviceManager& AudioEngine::deviceManager()
{
    return engine_->getDeviceManager();
}

void AudioEngine::setDefaultAudioDevice()
{
    auto& dm = engine_->getDeviceManager();
    dm.initialise(2, 2);

    // DeviceManager creates WaveInputDevice/WaveOutputDevice objects
    // asynchronously via triggerAsyncUpdate(). Pump the JUCE message
    // loop now so they exist before the UI enumerates them.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
}

void AudioEngine::restoreSavedAudioSettings()
{
    QSettings settings;
    if (!settings.contains("audio/sampleRate"))
        return;

    auto& adm = engine_->getDeviceManager().deviceManager;

    auto savedType = settings.value("audio/deviceType").toString();
    if (!savedType.isEmpty()) {
        auto& types = adm.getAvailableDeviceTypes();
        for (auto* type : types) {
            if (QString::fromStdString(type->getTypeName().toStdString()) == savedType) {
                adm.setCurrentAudioDeviceType(type->getTypeName(), true);
                break;
            }
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    adm.getAudioDeviceSetup(setup);

    auto savedOutput = settings.value("audio/outputDevice").toString();
    if (!savedOutput.isEmpty())
        setup.outputDeviceName = juce::String(savedOutput.toUtf8().constData());

    // An empty saved input means "None": no audio input is opened.
    if (settings.contains("audio/inputDevice")) {
        auto savedInput = settings.value("audio/inputDevice").toString();
        setup.inputDeviceName = juce::String(savedInput.toUtf8().constData());
        setup.useDefaultInputChannels = !savedInput.isEmpty();
        if (savedInput.isEmpty())
            setup.inputChannels.clear();
    }

    double sr = settings.value("audio/sampleRate", 0.0).toDouble();
    if (sr > 0.0)
        setup.sampleRate = sr;

    int buf = settings.value("audio/bufferSize", 0).toInt();
    if (buf > 0)
        setup.bufferSize = buf;

    adm.setAudioDeviceSetup(setup, true);
}

void AudioEngine::saveAudioSettings()
{
    auto& adm = engine_->getDeviceManager().deviceManager;
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    adm.getAudioDeviceSetup(setup);

    auto toQ = [](const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); };

    QSettings settings;
    settings.setValue("audio/deviceType", toQ(adm.getCurrentAudioDeviceType()));
    settings.setValue("audio/inputDevice", toQ(setup.inputDeviceName));
    settings.setValue("audio/outputDevice", toQ(setup.outputDeviceName));
    if (auto* device = adm.getCurrentAudioDevice()) {
        settings.setValue("audio/sampleRate", device->getCurrentSampleRate());
        settings.setValue("audio/bufferSize", device->getCurrentBufferSizeSamples());
    }
}

juce::StringArray AudioEngine::getAvailableInputDevices() const
{
    juce::StringArray result;
    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumWaveInDevices(); ++i)
        if (auto* dev = dm.getWaveInDevice(i))
            result.add(dev->getName());
    return result;
}

juce::StringArray AudioEngine::getAvailableOutputDevices() const
{
    juce::StringArray result;
    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumWaveOutDevices(); ++i)
        if (auto* dev = dm.getWaveOutDevice(i))
            result.add(dev->getName());
    return result;
}

juce::StringArray AudioEngine::getAvailableMidiInputDevices() const
{
    juce::StringArray result;
    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumMidiInDevices(); ++i) {
        auto dev = dm.getMidiInDevice(i);
        if (dev)
            result.add(dev->getName());
    }
    return result;
}

juce::StringArray AudioEngine::getAvailableMidiOutputDevices() const
{
    juce::StringArray result;
    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumMidiOutDevices(); ++i)
        if (auto dev = dm.getMidiOutDevice(i))
            result.add(dev->getName());
    return result;
}

void AudioEngine::applyMidiDeviceSettings()
{
    QSettings settings;
    const QStringList disabledInputs = settings.value("midi/disabledInputs").toStringList();

    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumMidiInDevices(); ++i) {
        if (auto dev = dm.getMidiInDevice(i)) {
            const QString name = QString::fromUtf8(dev->getName().toRawUTF8());
            // Only write when different, so applying never re-triggers a rescan.
            const bool enable = !disabledInputs.contains(name);
            if (dev->isEnabled() != enable)
                dev->setEnabled(enable);
            // Tracktion's default ("automatic") only plays live MIDI on
            // record-armed tracks; a controller should play its track at once.
            if (dev->getMonitorMode() != te::InputDevice::MonitorMode::on)
                dev->setMonitorMode(te::InputDevice::MonitorMode::on);
        }
    }

    if (!settings.contains("midi/enabledOutputs"))
        return;
    const QStringList enabledOutputs = settings.value("midi/enabledOutputs").toStringList();
    for (int i = 0; i < dm.getNumMidiOutDevices(); ++i) {
        if (auto* dev = dm.getMidiOutDevice(i)) {
            const QString name = QString::fromUtf8(dev->getName().toRawUTF8());
            const bool enable = enabledOutputs.contains(name);
            if (dev->isEnabled() != enable)
                dev->setEnabled(enable);
        }
    }
}

} // namespace OpenDaw
