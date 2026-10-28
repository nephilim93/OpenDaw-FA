#include "HumRecorder.h"
#include <cmath>

namespace OpenDaw {

namespace {
constexpr int kReadSamples = 4096;   // ~85 ms at 48 kHz: two periods of a low hum, and then some
const juce::String kTrackName = "Hum";
}

// ── LivePitchMonitor ─────────────────────────────────────────────────────────

LivePitchMonitor::LivePitchMonitor(te::Engine& engine) : engine_(engine) {}

LivePitchMonitor::~LivePitchMonitor() { stop(); }

bool LivePitchMonitor::start(const juce::String& waveInputName)
{
    stop();
    auto& dm = engine_.getDeviceManager();
    for (int i = 0; i < dm.getNumWaveInDevices(); ++i) {
        auto* dev = dm.getWaveInDevice(i);
        if (dev && dev->getName() == waveInputName && !dev->getChannels().empty()) {
            deviceChannel_ = dev->getChannels().front().indexInDevice;
            break;
        }
    }
    if (deviceChannel_ < 0 || dm.deviceManager.getCurrentAudioDevice() == nullptr)
        return false;
    fifo_.reset();
    recent_.assign(size_t(kReadSamples), 0.0f);
    dm.deviceManager.addAudioCallback(this);   // calls audioDeviceAboutToStart now
    running_ = true;
    return true;
}

void LivePitchMonitor::stop()
{
    if (!running_) return;
    engine_.getDeviceManager().deviceManager.removeAudioCallback(this);
    running_ = false;
    deviceChannel_ = -1;
    callbackChannel_ = -1;
}

void LivePitchMonitor::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // The callback only gets the active channels, packed together.
    int index = -1;
    if (device && deviceChannel_ >= 0) {
        const auto active = device->getActiveInputChannels();
        if (active[deviceChannel_]) {
            index = 0;
            for (int c = 0; c < deviceChannel_; ++c)
                if (active[c]) ++index;
        }
    }
    callbackChannel_ = index;
    sampleRate_ = device ? device->getCurrentSampleRate() : 0.0;
}

void LivePitchMonitor::audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                                        float* const*, int, int numSamples,
                                                        const juce::AudioIODeviceCallbackContext&)
{
    // Outputs are left alone: the device manager mixes and clears them.
    const int ch = callbackChannel_;
    if (ch < 0 || ch >= numInputs || inputs[ch] == nullptr) return;
    const auto scope = fifo_.write(std::min(numSamples, fifo_.getFreeSpace()));
    if (scope.blockSize1 > 0) std::copy_n(inputs[ch], scope.blockSize1, ring_.data() + scope.startIndex1);
    if (scope.blockSize2 > 0) std::copy_n(inputs[ch] + scope.blockSize1, scope.blockSize2, ring_.data() + scope.startIndex2);
}

Melody::Frame LivePitchMonitor::read()
{
    const int ready = fifo_.getNumReady();
    if (ready > 0) {
        const auto scope = fifo_.read(ready);
        auto append = [this](int start, int size) {
            if (size <= 0) return;
            recent_.insert(recent_.end(), ring_.begin() + start, ring_.begin() + start + size);
        };
        append(scope.startIndex1, scope.blockSize1);
        append(scope.startIndex2, scope.blockSize2);
        if (recent_.size() > size_t(kReadSamples))
            recent_.erase(recent_.begin(), recent_.end() - kReadSamples);
    }
    if (sampleRate_ <= 0.0) return {};
    return Melody::detectPitch(recent_, sampleRate_);
}

// ── HumRecorder ──────────────────────────────────────────────────────────────

HumRecorder::HumRecorder(EditManager& editMgr, QObject* parent)
    : QObject(parent), editMgr_(editMgr)
{
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &HumRecorder::poll);
    connect(&editMgr_, &EditManager::clipsRecorded, this, &HumRecorder::takeRecorded);
}

HumRecorder::~HumRecorder()
{
    if (monitor_) monitor_->stop();
}

te::AudioTrack* HumRecorder::track() const
{
    auto* edit = editMgr_.edit();
    if (!edit || !trackId_.isValid()) return nullptr;
    return dynamic_cast<te::AudioTrack*>(te::findTrackForID(*edit, trackId_));
}

QString HumRecorder::noteName(double hz)
{
    if (hz <= 0.0) return {};
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int note = int(std::lround(midi));
    const int cents = int(std::lround((midi - note) * 100.0));
    QString text = QString("%1%2").arg(names[((note % 12) + 12) % 12]).arg(note / 12 - 1);
    if (cents != 0) text += QString(" %1%2¢").arg(cents > 0 ? "+" : "").arg(cents);
    return text;
}

te::AudioTrack* HumRecorder::prepareTrack(QString* error)
{
    auto* edit = editMgr_.edit();
    if (!edit) return nullptr;

    // The input: the Hum track's own, else the one an armed track uses, else the first.
    auto isWave = [](te::InputDeviceInstance* in) {
        return in->getInputDevice().getDeviceType() == te::InputDevice::waveDevice;
    };
    te::AudioTrack* hum = nullptr;
    for (auto* t : editMgr_.getAudioTracks())
        if (t->getName() == kTrackName && !editMgr_.isMidiTrack(t) && !editMgr_.isBusTrack(t)) {
            hum = t;
            break;
        }

    edit->getTransport().ensureContextAllocated();
    te::InputDeviceInstance* input = nullptr;
    if (hum)
        for (auto* in : edit->getEditInputDevices().getDevicesForTargetTrack(*hum))
            if (isWave(in)) input = in;
    if (!input)
        for (auto* in : edit->getAllInputDevices())
            if (isWave(in))
                for (auto id : in->getTargets())
                    if (in->isRecordingEnabled(id)) input = in;
    if (!input)
        for (auto* in : edit->getAllInputDevices())
            if (isWave(in)) { input = in; break; }
    if (!input) {
        if (error) *error = "There is no audio input to record from. Choose one in Settings → Audio.";
        return nullptr;
    }

    if (!hum) {
        hum = editMgr_.addAudioTrack();
        if (!hum) return nullptr;
        hum->setName(kTrackName);
    }
    trackId_ = hum->itemID;

    // Keep the input on its other tracks too; only the Hum track records.
    // (Added to, not replaced: preparing twice must not forget the arming.)
    for (auto* in : edit->getAllInputDevices())
        for (auto id : in->getTargets())
            if (in->isRecordingEnabled(id)) {
                armedBefore_[id.getRawID()] = true;
                in->setRecordingEnabled(id, false);
            }
    if (!input->getTargets().contains(trackId_)) {
        auto res = input->setTarget(trackId_, false, &edit->getUndoManager(), 0);
        if (!res) {
            if (error) *error = QString::fromStdString(res.error().toStdString());
            return nullptr;
        }
    }
    input->setRecordingEnabled(trackId_, true);
    emit editMgr_.routingChanged();
    return hum;
}

bool HumRecorder::start(QString* error)
{
    auto* edit = editMgr_.edit();
    if (!edit || state_ != State::Idle) return false;
    auto& transport = edit->getTransport();
    if (transport.isRecording()) {
        if (error) *error = "Already recording.";
        return false;
    }
    auto* hum = prepareTrack(error);
    if (!hum) return false;

    // A count-in, so the take starts on the beat. The user's own count-in
    // wins; with none, one bar.
    if (edit->getCountInMode() == te::Edit::CountIn::none) {
        countInBefore_ = te::Edit::CountIn::none;
        edit->setCountInMode(te::Edit::CountIn::oneBar);
    }

    juce::String inputName;
    for (auto* in : edit->getEditInputDevices().getDevicesForTargetTrack(*hum))
        if (in->getInputDevice().getDeviceType() == te::InputDevice::waveDevice)
            inputName = in->getInputDevice().getName();
    monitor_ = std::make_unique<LivePitchMonitor>(edit->engine);
    monitor_->start(inputName);   // no readout without it, the take still records

    startSeconds_ = transport.getPosition().inSeconds();
    transport.record(false);
    if (!transport.isRecording()) {
        restore();
        if (error) *error = "Recording didn't start. Check the audio input in Settings → Audio.";
        return false;
    }
    setState(edit->getNumCountInBeats() > 0 ? State::CountIn : State::Recording);
    timer_.start();
    poll();
    return true;
}

void HumRecorder::stop()
{
    if (state_ == State::Idle) return;
    if (auto* edit = editMgr_.edit())
        edit->getTransport().stop(false, false);
    poll();
}

void HumRecorder::poll()
{
    auto* edit = editMgr_.edit();
    if (!edit || !edit->getTransport().isRecording()) {
        restore();
        return;
    }
    auto& transport = edit->getTransport();
    QString text;
    const double now = transport.getPosition().inSeconds();
    if (now < startSeconds_ - 0.01) {
        setState(State::CountIn);
        auto& ts = edit->tempoSequence;
        const double beatsLeft = ts.toBeats(tracktion::TimePosition::fromSeconds(startSeconds_)).inBeats()
                               - ts.toBeats(tracktion::TimePosition::fromSeconds(now)).inBeats();
        // Tracktion starts half a beat before the first click.
        const int beats = std::clamp(int(std::ceil(beatsLeft - 1.0e-3)), 1, std::max(1, edit->getNumCountInBeats()));
        text = QString("Count-in %1").arg(beats);
    } else {
        setState(State::Recording);
        const auto f = monitor_ && monitor_->isRunning() ? monitor_->read() : Melody::Frame{};
        text = f.hz > 0.0 ? noteName(f.hz) : QString::fromUtf8("…");
    }
    if (text != lastReadout_) {
        lastReadout_ = text;
        emit readoutChanged(text);
    }
}

void HumRecorder::restore()
{
    timer_.stop();
    if (monitor_) {
        monitor_->stop();
        monitor_.reset();
    }
    if (auto* edit = editMgr_.edit()) {
        if (countInBefore_) edit->setCountInMode(*countInBefore_);
        if (trackId_.isValid()) {
            for (auto* in : edit->getAllInputDevices())
                for (auto id : in->getTargets()) {
                    if (id == trackId_) in->setRecordingEnabled(id, false);
                    else if (armedBefore_.count(id.getRawID())) in->setRecordingEnabled(id, true);
                }
        }
    }
    countInBefore_.reset();
    armedBefore_.clear();
    if (!lastReadout_.isEmpty()) {
        lastReadout_.clear();
        emit readoutChanged({});
    }
    setState(State::Idle);
}

void HumRecorder::takeRecorded(const QList<te::Clip*>& clips)
{
    if (!trackId_.isValid()) return;
    te::WaveAudioClip* take = nullptr;
    for (auto* c : clips)
        if (auto* w = dynamic_cast<te::WaveAudioClip*>(c); w && w->getTrack() && w->getTrack()->itemID == trackId_)
            take = w;
    if (!take) return;
    restore();
    trackId_ = {};
    // After the timeline has picked the clip up.
    const auto clipId = take->itemID;
    QTimer::singleShot(0, this, [this, clipId]() {
        auto* edit = editMgr_.edit();
        if (!edit) return;
        if (auto* clip = dynamic_cast<te::WaveAudioClip*>(te::findClipForID(*edit, clipId)))
            emit editMgr_.humTakeRecorded(clip);
    });
}

void HumRecorder::setState(State s)
{
    if (s == state_) return;
    state_ = s;
    emit stateChanged(s);
}

} // namespace OpenDaw
