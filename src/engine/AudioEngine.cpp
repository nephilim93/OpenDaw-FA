#include "AudioEngine.h"
#include <QSettings>

namespace OpenDaw {

#if JUCE_LINUX
namespace {
// Tracktion opens the default device with every channel it reports before
// setDefaultAudioDevice() narrows it to stereo. Through PipeWire's ALSA
// plugin that is 128 capture channels, which crashes PipeWire's converter.
// Skip the automatic open; setDefaultAudioDevice() opens the device instead.
class OpenDawEngineBehaviour : public te::EngineBehaviour {
public:
    bool autoInitialiseDeviceManager() override { return false; }
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
}

AudioEngine::~AudioEngine() = default;

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

    double sr = settings.value("audio/sampleRate", 0.0).toDouble();
    if (sr > 0.0)
        setup.sampleRate = sr;

    int buf = settings.value("audio/bufferSize", 0).toInt();
    if (buf > 0)
        setup.bufferSize = buf;

    adm.setAudioDeviceSetup(setup, true);
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

void AudioEngine::enableAllMidiInputDevices()
{
    auto& dm = engine_->getDeviceManager();
    for (int i = 0; i < dm.getNumMidiInDevices(); ++i) {
        auto dev = dm.getMidiInDevice(i);
        if (dev)
            dev->setEnabled(true);
    }
}

} // namespace OpenDaw
