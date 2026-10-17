#include "MidiOutPlugin.h"

namespace OpenDaw {

namespace {
const juce::Identifier kDestinationId("destination");
const juce::Identifier kChannelId("channel");
}

const char* MidiOutPlugin::xmlTypeName = "openDawMidiOut";

MidiOutPlugin::MidiOutPlugin(te::PluginCreationInfo info)
    : te::Plugin(info)
{
    destination_.referTo(state, kDestinationId, getUndoManager());
    channel_.referTo(state, kChannelId, getUndoManager(), 0);
    auto* ports = VirtualMidiPorts::instance();
    // A new one sends to OpenDaw's own port where there is one (not an undo step).
    if (destination_.get().isEmpty() && ports && ports->isEnabled())
        destination_.setValue(VirtualMidiPorts::kVirtualDestination, nullptr);
    sendChannel_ = juce::jlimit(0, 16, channel_.get());
    if (ports) ports->addQueue(&queue_, destination_.get());
}

MidiOutPlugin::~MidiOutPlugin()
{
    notifyListenersOfDeletion();
    if (auto* ports = VirtualMidiPorts::instance()) ports->removeQueue(&queue_);
}

void MidiOutPlugin::setDestination(const juce::String& destination, const juce::String& undoName)
{
    if (auto* um = getUndoManager(); um && undoName.isNotEmpty())
        um->beginNewTransaction(undoName);
    destination_ = destination;
}

void MidiOutPlugin::setChannel(int channel, const juce::String& undoName)
{
    if (auto* um = getUndoManager(); um && undoName.isNotEmpty())
        um->beginNewTransaction(undoName);
    channel_ = juce::jlimit(0, 16, channel);
}

juce::String MidiOutPlugin::problem() const
{
    auto* ports = VirtualMidiPorts::instance();
    return ports ? ports->destinationProblem(destination()) : juce::String("MIDI is not running.");
}

void MidiOutPlugin::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues(v, destination_, channel_);
    sendChannel_ = juce::jlimit(0, 16, channel_.get());
    if (auto* ports = VirtualMidiPorts::instance()) ports->setDestination(&queue_, destination_.get());
}

void MidiOutPlugin::valueTreePropertyChanged(juce::ValueTree& v, const juce::Identifier& id)
{
    te::Plugin::valueTreePropertyChanged(v, id);
    // te::Plugin's listener runs before the cached values' own, so on
    // undo/redo they still hold the old value unless refreshed here.
    if (v == state && id == kDestinationId) {
        destination_.forceUpdateOfCachedValue();
        if (auto* ports = VirtualMidiPorts::instance()) ports->setDestination(&queue_, destination_.get());
    } else if (v == state && id == kChannelId) {
        channel_.forceUpdateOfCachedValue();
        sendChannel_ = juce::jlimit(0, 16, channel_.get());
    }
}

void MidiOutPlugin::push(const juce::MidiMessage& m, double dueMs)
{
    const int ch = sendChannel_.load();
    if (ch > 0 && m.getChannel() > 0) {
        auto copy = m;
        copy.setChannel(ch);
        queue_.push(copy, dueMs);
    } else {
        queue_.push(m, dueMs);
    }
}

void MidiOutPlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    // Nothing to hear from this track: the other app plays it.
    if (fc.destBuffer) fc.destBuffer->clear(fc.bufferStartSample, fc.bufferNumSamples);
    auto* midi = fc.bufferForMidiMessages;
    if (!midi) return;

    const double blockStart = juce::Time::getMillisecondCounterHiRes();
    if (midi->isAllNotesOff) {
        // Stop, or a jump: silence whatever the other side is holding.
        const int ch = sendChannel_.load();
        for (int c = ch > 0 ? ch : 1; c <= (ch > 0 ? ch : 16); ++c) {
            queue_.push(juce::MidiMessage::controllerEvent(c, 64, 0), blockStart);
            queue_.push(juce::MidiMessage::allNotesOff(c), blockStart);
        }
    }
    midi->sortByTimestamp();
    for (auto& m : *midi)
        push(m, blockStart + std::max(0.0, m.getTimeStamp()) * 1000.0);
    midi->clear();
}

} // namespace OpenDaw
