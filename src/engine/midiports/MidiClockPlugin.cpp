#include "MidiClockPlugin.h"

namespace OpenDaw {

const char* MidiClockPlugin::xmlTypeName = "openDawMidiClock";

MidiClockPlugin::MidiClockPlugin(te::PluginCreationInfo info)
    : te::Plugin(info)
{
    if (auto* ports = VirtualMidiPorts::instance())
        ports->addQueue(&queue_, VirtualMidiPorts::kVirtualDestination);
}

MidiClockPlugin::~MidiClockPlugin()
{
    notifyListenersOfDeletion();
    if (auto* ports = VirtualMidiPorts::instance()) ports->removeQueue(&queue_);
}

void MidiClockPlugin::initialise(const te::PluginInitialisationInfo&)
{
    auto fresh = std::make_unique<tracktion::tempo::Sequence::Position>(te::createPosition(edit.tempoSequence));
    const juce::SpinLock::ScopedLockType sl(positionLock_);
    position_ = std::move(fresh);
    needsPosition_ = true;
}

void MidiClockPlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    // Audio passes through: nothing to do to it.
    auto* ports = VirtualMidiPorts::instance();
    const bool sending = ports && ports->isEnabled() && ports->clockEnabled();
    const double now = juce::Time::getMillisecondCounterHiRes();

    const juce::SpinLock::ScopedTryLockType sl(positionLock_);
    if (!sl.isLocked() || !position_) return;

    const bool playing = fc.isPlaying;
    if (playing != wasPlaying_) {
        wasPlaying_ = playing;
        needsPosition_ = true;
        if (!playing && sending) push(juce::MidiMessage::midiStop(), now);
    }
    if (!playing) return;

    // Same rules as Tracktion's own clock generator: 24 ticks per beat, and
    // a song position with start (from the top) or continue after a start,
    // a jump or a loop.
    const double blockStart = fc.editTime.getStart().inSeconds();
    const double blockLength = fc.editTime.getLength().inSeconds();
    if (blockLength <= 0.0) return;
    position_->set(fc.editTime.getStart());
    const double startPpq = position_->getPPQTime();
    position_->set(fc.editTime.getEnd());
    const double endPpq = position_->getPPQTime();

    const bool jumped = std::abs(lastBlockEndPpq_ - endPpq) > 0.4 && lastBlockEndPpq_ != 0.0;
    const bool looped = blockStart < lastBlockStart_;
    lastBlockEndPpq_ = endPpq;
    lastBlockStart_ = blockStart;
    needsPosition_ = needsPosition_ || jumped || looped;

    const double startTick = startPpq * 24.0;
    const double endTick = endPpq * 24.0;
    if (endTick <= startTick) return;
    const double secondsPerTick = blockLength / (endTick - startTick);
    for (auto tick = (long long) std::ceil(startTick - 1.0e-6); double(tick) < endTick; ++tick) {
        const double due = now + (double(tick) - startTick) * secondsPerTick * 1000.0;
        if (needsPosition_) {
            // Song position counts sixteenths: only whole ones can be sent,
            // so wait for the next beat (as Tracktion does).
            if (tick % 24 != 0) continue;
            if (sending) {
                push(juce::MidiMessage::songPositionPointer(int(tick / 6)), due);
                push(tick == 0 ? juce::MidiMessage::midiStart() : juce::MidiMessage::midiContinue(), due);
            }
            needsPosition_ = false;
        }
        if (sending) push(juce::MidiMessage::midiClock(), due);
    }
}

} // namespace OpenDaw
