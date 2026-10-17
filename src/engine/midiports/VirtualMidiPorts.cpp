#include "VirtualMidiPorts.h"
#include <QSettings>
#include <algorithm>

namespace OpenDaw {

namespace {
constexpr int kSenderIntervalMs = 1;
// Events this close to due are sent now rather than a tick late.
constexpr double kSendAheadMs = 0.5;

std::atomic<VirtualMidiPorts*> gInstance{ nullptr };

QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
} // namespace

// ── MidiSendQueue ───────────────────────────────────────────────────────────

bool MidiSendQueue::push(const juce::MidiMessage& m, double dueMs)
{
    const int size = m.getRawDataSize();
    if (size <= 0 || size > 3) return false;
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 <= 0) return false;
    auto& e = events_[size_t(scope.startIndex1)];
    std::copy_n(m.getRawData(), size, e.bytes.begin());
    e.size = size;
    e.dueMs = dueMs;
    return true;
}

// ── VirtualMidiPorts ────────────────────────────────────────────────────────

VirtualMidiPorts::VirtualMidiPorts(te::Engine& engine)
    : juce::Thread("OpenDaw MIDI out"), engine_(engine)
{
    engine_.getDeviceManager().addChangeListener(this);
    clock_ = QSettings().value(kClockKey, true).toBool();
    startThread(juce::Thread::Priority::high);
    gInstance = this;
}

VirtualMidiPorts* VirtualMidiPorts::instance()
{
    return gInstance.load();
}

VirtualMidiPorts::~VirtualMidiPorts()
{
    gInstance = nullptr;
    stopThread(1000);
    engine_.getDeviceManager().removeChangeListener(this);
    setEnabled(false);
    const juce::ScopedLock sl(lock_);
    outputs_.clear();
}

bool VirtualMidiPorts::supported()
{
#if JUCE_MAC || JUCE_LINUX
    return true;
#else
    return false;
#endif
}

bool VirtualMidiPorts::enabledInSettings()
{
    return supported() && QSettings().value(kEnabledKey, true).toBool();
}

void VirtualMidiPorts::setEnabled(bool enabled)
{
    if (!supported() || enabled == isEnabled()) return;
    auto& dm = engine_.getDeviceManager();

    if (enabled) {
        virtualIn_ = juce::MidiInput::createNewDevice(kInName, this);
        if (!virtualIn_) {
            juce::Logger::writeToLog("Virtual MIDI: could not create '" + juce::String(kInName) + "'");
            return;
        }
        virtualIn_->start();
        {
            const juce::ScopedLock sl(lock_);
            virtualOut_ = juce::MidiOutput::createNewDevice(kOutName);
            if (!virtualOut_) juce::Logger::writeToLog("Virtual MIDI: could not create '" + juce::String(kOutName) + "'");
        }
        // The input reaches tracks through a Tracktion virtual input of the
        // same name. Tracktion keeps those names in its settings and makes
        // the devices on its next scan, which may not have run yet: so add
        // the name if it's missing (that rescans), else rescan anyway.
        if (dm.createVirtualMidiDevice(kInName).failed())
            dm.rescanMidiDeviceList();
        findTracktionInput();   // and again when the rescan reports a change
        return;
    }

    if (virtualIn_) virtualIn_->stop();
    virtualIn_.reset();
    {
        const juce::ScopedLock sl(lock_);
        virtualOut_.reset();
    }
    {
        const juce::SpinLock::ScopedLockType sl(inputLock_);
        tracktionIn_.reset();
    }
    for (auto& d : dm.getMidiInDevices())
        if (d->getDeviceType() == te::InputDevice::virtualMidiDevice && d->getName() == kInName)
            if (auto* v = dynamic_cast<te::VirtualMidiInputDevice*>(d.get())) {
                dm.deleteVirtualMidiDevice(*v);
                break;
            }
}

void VirtualMidiPorts::setClockEnabled(bool on)
{
    QSettings().setValue(kClockKey, on);
    clock_ = on;
}

bool VirtualMidiPorts::isOwnPortSeenAsDevice(const juce::String& deviceName, bool isInput)
{
    if (!supported()) return false;
    // Our "Out" is a source (an input to everyone); our "In" a destination.
    return isInput ? deviceName == kOutName : deviceName == kInName;
}

QStringList VirtualMidiPorts::destinations() const
{
    QStringList list;
    if (isEnabled()) list << kVirtualDestination;
    for (const auto& info : juce::MidiOutput::getAvailableDevices())
        if (!isOwnPortSeenAsDevice(info.name, false) && !list.contains(toQ(info.name)))
            list << toQ(info.name);
    return list;
}

juce::String VirtualMidiPorts::destinationProblem(const juce::String& destination) const
{
    if (destination.isEmpty()) return "No destination chosen.";
    if (destination == kVirtualDestination) {
        if (!supported())
            return "This system has no virtual MIDI ports; send to a loopMIDI port instead.";
        if (!isEnabled()) return "OpenDaw's virtual ports are switched off (Settings > MIDI).";
        return {};
    }
    for (const auto& info : juce::MidiOutput::getAvailableDevices())
        if (info.name == destination) return {};
    return "'" + destination + "' isn't connected.";
}

void VirtualMidiPorts::findTracktionInput()
{
    std::shared_ptr<te::MidiInputDevice> found;
    if (isEnabled())
        for (auto& d : engine_.getDeviceManager().getMidiInDevices())
            if (d->getDeviceType() == te::InputDevice::virtualMidiDevice && d->getName() == kInName) {
                found = d;
                break;
            }
    const juce::SpinLock::ScopedLockType sl(inputLock_);
    tracktionIn_ = found;
}

void VirtualMidiPorts::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // A rescan rebuilds Tracktion's device objects.
    findTracktionInput();
    const juce::ScopedLock sl(lock_);
    for (auto* q : queues_) openOutput(q->destination_);
}

void VirtualMidiPorts::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m)
{
    // CoreMIDI / ALSA thread.
    ++received_;
    std::shared_ptr<te::MidiInputDevice> target;
    {
        const juce::SpinLock::ScopedLockType sl(inputLock_);
        target = tracktionIn_;
    }
    if (target) {
        target->handleIncomingMidiMessage(m, sourceId_);
        ++forwarded_;
    }
}

// ── Sending ─────────────────────────────────────────────────────────────────

void VirtualMidiPorts::addQueue(MidiSendQueue* queue, const juce::String& destination)
{
    const juce::ScopedLock sl(lock_);
    if (std::find(queues_.begin(), queues_.end(), queue) == queues_.end()) queues_.push_back(queue);
    queue->destination_ = destination;
    openOutput(destination);
}

void VirtualMidiPorts::setDestination(MidiSendQueue* queue, const juce::String& destination)
{
    const juce::ScopedLock sl(lock_);
    queue->destination_ = destination;
    openOutput(destination);
    closeUnusedOutputs();
}

void VirtualMidiPorts::removeQueue(MidiSendQueue* queue)
{
    const juce::ScopedLock sl(lock_);
    queues_.erase(std::remove(queues_.begin(), queues_.end(), queue), queues_.end());
    closeUnusedOutputs();
}

void VirtualMidiPorts::openOutput(const juce::String& destination)
{
    // Called with lock_ held.
    if (destination.isEmpty() || destination == kVirtualDestination) return;
    if (auto it = outputs_.find(destination); it != outputs_.end() && it->second) return;
    for (const auto& info : juce::MidiOutput::getAvailableDevices())
        if (info.name == destination && !isOwnPortSeenAsDevice(info.name, false)) {
            outputs_[destination] = juce::MidiOutput::openDevice(info.identifier);
            if (!outputs_[destination])
                juce::Logger::writeToLog("MIDI Out: could not open '" + destination + "'");
            return;
        }
}

void VirtualMidiPorts::closeUnusedOutputs()
{
    // Called with lock_ held.
    for (auto it = outputs_.begin(); it != outputs_.end();) {
        const bool used = std::any_of(queues_.begin(), queues_.end(),
                                      [&](MidiSendQueue* q) { return q->destination_ == it->first; });
        it = used ? std::next(it) : outputs_.erase(it);
    }
}

juce::MidiOutput* VirtualMidiPorts::outputFor(const juce::String& destination)
{
    // Called with lock_ held.
    if (destination == kVirtualDestination) return virtualOut_.get();
    auto it = outputs_.find(destination);
    return it != outputs_.end() ? it->second.get() : nullptr;
}

void VirtualMidiPorts::run()
{
    struct Due {
        MidiSendQueue::Event event;
        juce::MidiOutput* out;
    };
    std::vector<Due> due;
    due.reserve(512);

    while (!threadShouldExit()) {
        wait(kSenderIntervalMs);
        const double now = juce::Time::getMillisecondCounterHiRes();
        due.clear();
        {
            const juce::ScopedLock sl(lock_);
            for (auto* q : queues_) {
                auto* out = outputFor(q->destination_);
                // Events leave in order, so stop at the first not yet due.
                while (q->fifo_.getNumReady() > 0) {
                    int start1, size1, start2, size2;
                    q->fifo_.prepareToRead(1, start1, size1, start2, size2);   // peek
                    const auto& e = q->events_[size_t(start1)];
                    if (e.dueMs > now + kSendAheadMs) break;
                    if (out) due.push_back({ e, out });
                    q->fifo_.finishedRead(1);
                }
            }
            std::stable_sort(due.begin(), due.end(),
                             [](const Due& a, const Due& b) { return a.event.dueMs < b.event.dueMs; });
            for (const auto& d : due)
                d.out->sendMessageNow(juce::MidiMessage(d.event.bytes.data(), d.event.size));
        }
    }
}

} // namespace OpenDaw
