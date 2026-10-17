#include "AudioSettingsPage.h"
#include <QVBoxLayout>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSignalBlocker>

namespace OpenDaw {

namespace {
QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
juce::String toJ(const QString& s) { return juce::String(s.toUtf8().constData()); }
}

AudioSettingsPage::AudioSettingsPage(AudioEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine)
{
    setAccessibleName("Audio Settings");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Audio Device", this);
    group->setAccessibleName("Audio Device Settings");
    auto* form = new QFormLayout(group);

    deviceTypeCombo_ = new QComboBox(this);
    deviceTypeCombo_->setAccessibleName("Audio device type");
    form->addRow("Driver:", deviceTypeCombo_);

    inputCombo_ = new QComboBox(this);
    inputCombo_->setAccessibleName("Input device");
    inputCombo_->setToolTip("Device used for recording. Choose None if you never record audio.");
    form->addRow("Input Device:", inputCombo_);

    outputCombo_ = new QComboBox(this);
    outputCombo_->setAccessibleName("Output device");
    form->addRow("Output Device:", outputCombo_);

    sampleRateCombo_ = new QComboBox(this);
    sampleRateCombo_->setAccessibleName("Sample rate");
    form->addRow("Sample Rate:", sampleRateCombo_);

    bufferSizeCombo_ = new QComboBox(this);
    bufferSizeCombo_->setAccessibleName("Buffer size");
    form->addRow("Buffer Size:", bufferSizeCombo_);

    latencyLabel_ = new QLabel(this);
    latencyLabel_->setAccessibleName("Latency");
    form->addRow("Latency:", latencyLabel_);

    allChannels_ = new QCheckBox("Use every channel of the device", this);
    allChannels_->setAccessibleName("Use every channel");
    allChannels_->setToolTip("Open all inputs and outputs, not just the first pair: tracks can then "
                             "send to and record from the others");
    allChannels_->setChecked(AudioEngine::allChannelsInSettings());
    form->addRow("Channels:", allChannels_);
    connect(allChannels_, &QCheckBox::toggled, this, [this](bool on) {
        engine_.setAllChannels(on);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        engine_.saveAudioSettings();
        if (onDeviceChanged_) onDeviceChanged_();
        populateDevices();
    });

    mainLayout->addWidget(group);

    // Audio between OpenDaw and other apps on this computer (VCV Rack, ...).
    auto* appsNote = new QLabel(this);
    appsNote->setAccessibleName("Connecting other apps");
    appsNote->setWordWrap(true);
    appsNote->setStyleSheet("font-size: 10px; color: gray; padding: 4px 8px;");
#if JUCE_LINUX
    appsNote->setText("Other apps: choose the JACK driver (PipeWire provides it too). OpenDaw's channels "
                      "then appear as ports that apps like VCV Rack connect to. Turn on \"Use every "
                      "channel\" and send tracks to Output 3 + 4 and up.");
#elif JUCE_MAC
    appsNote->setText("Other apps: install a loopback driver such as BlackHole (free). In Audio MIDI Setup, "
                      "make an Aggregate Device of your speakers and BlackHole, choose it here, and turn "
                      "on \"Use every channel\". Send a track to BlackHole's outputs to feed another app "
                      "(VCV Rack's Audio module), and record its return on BlackHole's inputs.");
#else
    appsNote->setText("Other apps: install a loopback driver such as VB-CABLE, or use an ASIO driver with "
                      "loopback. Turn on \"Use every channel\" and send a track to the cable's outputs to "
                      "feed another app, and record its return on the cable's inputs.");
#endif
    mainLayout->addWidget(appsNote);

    errorLabel_ = new QLabel(this);
    errorLabel_->setAccessibleName("Audio device error");
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet("color: #e06060; padding: 4px 8px;");
    errorLabel_->hide();
    mainLayout->addWidget(errorLabel_);

    driverNote_ = new QLabel(this);
    driverNote_->setAccessibleName("Driver information note");
    driverNote_->setWordWrap(true);
    driverNote_->setStyleSheet("font-size: 10px; color: gray; padding: 4px 8px;");
    mainLayout->addWidget(driverNote_);
    mainLayout->addStretch();

    connect(deviceTypeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        auto& adm = engine_.deviceManager().deviceManager;
        auto& types = adm.getAvailableDeviceTypes();
        if (idx < 0 || idx >= types.size()) return;
        adm.setCurrentAudioDeviceType(types[idx]->getTypeName(), true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        engine_.applyChannelSetting();
        populateDevices();
        updateDriverNote();
        engine_.saveAudioSettings();
        if (onDeviceChanged_) onDeviceChanged_();
    });

    connect(inputCombo_, &QComboBox::activated, this, [this]() {
        const QString name = inputCombo_->currentData().toString();
        applySetup([&](auto& setup) {
            setup.inputDeviceName = toJ(name);
            setup.useDefaultInputChannels = !name.isEmpty();
            if (name.isEmpty())
                setup.inputChannels.clear();
        });
    });

    connect(outputCombo_, &QComboBox::activated, this, [this]() {
        const QString name = outputCombo_->currentText();
        applySetup([&](auto& setup) { setup.outputDeviceName = toJ(name); });
    });

    connect(sampleRateCombo_, &QComboBox::activated, this, [this]() {
        const double rate = sampleRateCombo_->currentData().toDouble();
        applySetup([&](auto& setup) { setup.sampleRate = rate; });
    });

    connect(bufferSizeCombo_, &QComboBox::activated, this, [this]() {
        const int size = bufferSizeCombo_->currentData().toInt();
        applySetup([&](auto& setup) { setup.bufferSize = size; });
    });

    populateDeviceTypes();
}

// Applies one change to the current device setup, then refreshes the page
// and saves the result. Errors are shown on the page.
void AudioSettingsPage::applySetup(
    const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup&)>& change)
{
    auto& adm = engine_.deviceManager().deviceManager;
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    adm.getAudioDeviceSetup(setup);
    change(setup);

    auto error = adm.setAudioDeviceSetup(setup, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    // A new device opens its first pair: keep the channel choice.
    if (error.isEmpty()) engine_.applyChannelSetting();

    if (error.isNotEmpty()) {
        errorLabel_->setText("Could not apply the audio settings: " + toQ(error));
        errorLabel_->show();
    } else {
        errorLabel_->hide();
        engine_.saveAudioSettings();
        if (onDeviceChanged_) onDeviceChanged_();
    }

    populateDevices();
}

void AudioSettingsPage::populateDeviceTypes()
{
    QSignalBlocker block(deviceTypeCombo_);
    deviceTypeCombo_->clear();

    auto& adm = engine_.deviceManager().deviceManager;
    auto& types = adm.getAvailableDeviceTypes();
    auto* currentType = adm.getCurrentDeviceTypeObject();

    int selectedIdx = 0;
    for (int i = 0; i < types.size(); ++i) {
        deviceTypeCombo_->addItem(toQ(types[i]->getTypeName()));
        if (currentType && currentType->getTypeName() == types[i]->getTypeName())
            selectedIdx = i;
    }
    deviceTypeCombo_->setCurrentIndex(selectedIdx);

    populateDevices();
    updateDriverNote();
}

void AudioSettingsPage::populateDevices()
{
    QSignalBlocker blockIn(inputCombo_);
    QSignalBlocker blockOut(outputCombo_);
    inputCombo_->clear();
    outputCombo_->clear();

    auto& adm = engine_.deviceManager().deviceManager;
    auto* type = adm.getCurrentDeviceTypeObject();
    if (type) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup(setup);

        // Some drivers (e.g. ASIO) use one device for input and output.
        if (type->hasSeparateInputsAndOutputs()) {
            inputCombo_->setEnabled(true);
            inputCombo_->addItem("None", QString());
            for (const auto& name : type->getDeviceNames(true)) {
                inputCombo_->addItem(toQ(name), toQ(name));
                if (name == setup.inputDeviceName)
                    inputCombo_->setCurrentIndex(inputCombo_->count() - 1);
            }
        } else {
            inputCombo_->addItem("Same as output device", QString());
            inputCombo_->setEnabled(false);
        }

        for (const auto& name : type->getDeviceNames(false)) {
            outputCombo_->addItem(toQ(name));
            if (name == setup.outputDeviceName)
                outputCombo_->setCurrentIndex(outputCombo_->count() - 1);
        }
    }

    populateSampleRates();
    populateBufferSizes();
    updateLatency();
}

void AudioSettingsPage::populateSampleRates()
{
    QSignalBlocker block(sampleRateCombo_);
    sampleRateCombo_->clear();

    auto* device = engine_.deviceManager().deviceManager.getCurrentAudioDevice();
    if (!device) return;

    const double currentRate = device->getCurrentSampleRate();
    for (double rate : device->getAvailableSampleRates()) {
        sampleRateCombo_->addItem(QString("%1 Hz").arg(static_cast<int>(rate)), rate);
        if (std::abs(rate - currentRate) < 1.0)
            sampleRateCombo_->setCurrentIndex(sampleRateCombo_->count() - 1);
    }
}

void AudioSettingsPage::populateBufferSizes()
{
    QSignalBlocker block(bufferSizeCombo_);
    bufferSizeCombo_->clear();

    auto* device = engine_.deviceManager().deviceManager.getCurrentAudioDevice();
    if (!device) return;

    const int currentSize = device->getCurrentBufferSizeSamples();
    double sr = device->getCurrentSampleRate();
    if (sr <= 0.0) sr = 44100.0;

    for (int size : device->getAvailableBufferSizes()) {
        const double ms = size / sr * 1000.0;
        bufferSizeCombo_->addItem(
            QString("%1 samples (%2 ms)").arg(size).arg(ms, 0, 'f', 1), size);
        if (size == currentSize)
            bufferSizeCombo_->setCurrentIndex(bufferSizeCombo_->count() - 1);
    }
}

// Round-trip latency as reported by the driver: buffer plus device latency
// on each side.
void AudioSettingsPage::updateLatency()
{
    auto* device = engine_.deviceManager().deviceManager.getCurrentAudioDevice();
    if (!device || device->getCurrentSampleRate() <= 0.0) {
        latencyLabel_->setText("No audio device open");
        return;
    }

    const double sr = device->getCurrentSampleRate();
    const int buffer = device->getCurrentBufferSizeSamples();
    const double outMs = (buffer + device->getOutputLatencyInSamples()) / sr * 1000.0;
    const double inMs = (buffer + device->getInputLatencyInSamples()) / sr * 1000.0;

    if (device->getActiveInputChannels().isZero())
        latencyLabel_->setText(QString("Output %1 ms").arg(outMs, 0, 'f', 1));
    else
        latencyLabel_->setText(QString("Input %1 ms, output %2 ms (round trip %3 ms)")
                                   .arg(inMs, 0, 'f', 1).arg(outMs, 0, 'f', 1)
                                   .arg(inMs + outMs, 0, 'f', 1));
}

void AudioSettingsPage::updateDriverNote()
{
    auto* type = engine_.deviceManager().deviceManager.getCurrentDeviceTypeObject();
    if (!type) {
        driverNote_->clear();
        return;
    }

    const QString typeName = toQ(type->getTypeName());
    if (typeName.contains("WASAPI", Qt::CaseInsensitive)) {
        driverNote_->setText(
            "Windows Audio (WASAPI) uses the sample rate configured in Windows "
            "Sound Settings. For more sample rate options, switch to an ASIO driver "
            "or change the rate in Windows Settings > Sound > Device Properties.");
    } else if (typeName.contains("ASIO", Qt::CaseInsensitive)) {
        driverNote_->setText(
            "ASIO drivers provide low-latency access with flexible sample rate "
            "and buffer size options.");
    } else if (typeName.contains("DirectSound", Qt::CaseInsensitive)) {
        driverNote_->setText(
            "DirectSound has higher latency than WASAPI or ASIO. "
            "Consider switching to WASAPI or an ASIO driver for better performance.");
    } else {
        driverNote_->clear();
    }
}

} // namespace OpenDaw
