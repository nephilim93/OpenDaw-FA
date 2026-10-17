// OpenDaw's virtual MIDI ports and the MIDI Out instrument, against real
// CoreMIDI / ALSA ports: this test plays the other app, sending to
// "OpenDaw In" and listening on "OpenDaw Out".
#include "app/JuceQtBridge.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/midiports/MidiClockPlugin.h"
#include "engine/midiports/MidiOutPlugin.h"
#include "engine/midiports/VirtualMidiPorts.h"
#include "ui/dialogs/MidiOutDialog.h"
#include <QApplication>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <QComboBox>
#include <QElapsedTimer>
#include <QSettings>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace OpenDaw;

namespace {

int failures = 0;

#define EXPECT(cond)                                                              \
    do {                                                                          \
        if (cond) std::printf("  ok    %s\n", #cond);                             \
        else { std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__); ++failures; } \
    } while (0)

// Qt's offscreen platform doesn't spin the macOS run loop, which is where
// CoreMIDI delivers device changes; the app's Cocoa loop does, so do it here.
void pump()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
#if JUCE_MAC
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
#endif
}

void settle(int ms = 50)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        pump();
}

template <typename Pred>
bool waitFor(Pred pred, int ms = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return false;
        pump();
    }
    return true;
}

bool hasDevice(const juce::Array<juce::MidiDeviceInfo>& list, const juce::String& name)
{
    for (const auto& d : list)
        if (d.name == name) return true;
    return false;
}

juce::String identifierOf(const juce::Array<juce::MidiDeviceInfo>& list, const juce::String& name)
{
    for (const auto& d : list)
        if (d.name == name) return d.identifier;
    return {};
}

// The other app's side of "OpenDaw Out".
struct Listener : juce::MidiInputCallback {
    struct Got {
        juce::MidiMessage message;
        double atMs;
    };
    std::mutex lock;
    std::vector<Got> got;

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override
    {
        const std::scoped_lock sl(lock);
        got.push_back({ m, juce::Time::getMillisecondCounterHiRes() });
    }
    std::vector<Got> snapshot()
    {
        const std::scoped_lock sl(lock);
        return got;
    }
    void clear()
    {
        const std::scoped_lock sl(lock);
        got.clear();
    }
};

void renderBlock(MidiOutPlugin& plugin, te::MidiMessageArray& midi, int numSamples = 512)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    buffer.clear();
    te::PluginRenderContext rc(&buffer, juce::AudioChannelSet::stereo(), 0, numSamples, &midi, 0.0,
                               {}, true, false, false, false);
    plugin.applyToBuffer(rc);
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Own settings, so the user's MIDI choices are never touched.
    QCoreApplication::setOrganizationName("OpenDawTests");
    QCoreApplication::setApplicationName("MidiPortsTest");
    QSettings().clear();
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (!VirtualMidiPorts::supported()) {
        std::printf("Virtual MIDI ports aren't supported on this system; nothing to test.\n");
        return 0;
    }

    JuceQtBridge bridge;
    bridge.start();
    AudioEngine engine;
    engine.setDefaultAudioDevice();
    EditManager editMgr(engine);
    editMgr.stopAutosave();
    auto& ports = engine.virtualMidiPorts();
    auto& um = editMgr.edit()->getUndoManager();

    // ── The ports exist for other apps ──
    std::printf("ports published\n");
    EXPECT(ports.isEnabled());
    EXPECT(waitFor([] { return hasDevice(juce::MidiOutput::getAvailableDevices(), VirtualMidiPorts::kInName); }));
    EXPECT(waitFor([] { return hasDevice(juce::MidiInput::getAvailableDevices(), VirtualMidiPorts::kOutName); }));
    settle(300);   // Tracktion's rescan

    // ── OpenDaw's own ports never show up as its devices (no loops) ──
    std::printf("own ports hidden\n");
    EXPECT(!engine.getAvailableMidiInputDevices().contains(VirtualMidiPorts::kOutName));
    EXPECT(!engine.getAvailableMidiOutputDevices().contains(VirtualMidiPorts::kInName));
    bool offersIn = false, offersOut = false;
    for (const auto& src : editMgr.getAvailableMidiInputSources()) {
        offersIn |= src.deviceName == VirtualMidiPorts::kInName;
        offersOut |= src.deviceName == VirtualMidiPorts::kOutName;
    }
    EXPECT(offersIn);
    EXPECT(!offersOut);
    auto& dm = engine.deviceManager();
    for (int i = 0; i < dm.getNumMidiInDevices(); ++i)
        if (auto d = dm.getMidiInDevice(i); d && d->getName() == VirtualMidiPorts::kOutName)
            EXPECT(!d->isEnabled());
    for (int i = 0; i < dm.getNumMidiOutDevices(); ++i)
        if (auto* d = dm.getMidiOutDevice(i); d && d->getName() == VirtualMidiPorts::kInName)
            EXPECT(!d->isEnabled());
    EXPECT(ports.destinations().value(0) == VirtualMidiPorts::kVirtualDestination);
    EXPECT(!ports.destinations().contains(VirtualMidiPorts::kInName));

    // ── Another app plays OpenDaw through "OpenDaw In" ──
    std::printf("receive on OpenDaw In\n");
    auto* inTrack = editMgr.addMidiTrack();
    editMgr.assignMidiInputToTrack(*inTrack, VirtualMidiPorts::kInName);
    EXPECT(waitFor([&] { return editMgr.getTrackInputName(inTrack) == VirtualMidiPorts::kInName; }));
    {
        auto toOpenDaw = juce::MidiOutput::openDevice(identifierOf(juce::MidiOutput::getAvailableDevices(),
                                                                   VirtualMidiPorts::kInName));
        EXPECT(toOpenDaw != nullptr);
        const int before = ports.receivedCount();
        const int forwardedBefore = ports.forwardedCount();
        if (toOpenDaw) {
            toOpenDaw->sendMessageNow(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)));
            toOpenDaw->sendMessageNow(juce::MidiMessage::noteOff(1, 60));
        }
        EXPECT(waitFor([&] { return ports.receivedCount() >= before + 2; }));
        EXPECT(ports.forwardedCount() >= forwardedBefore + 2);   // into the track's input
    }

    // ── A MIDI Out track plays another app through "OpenDaw Out" ──
    std::printf("send on OpenDaw Out\n");
    Listener listener;
    auto fromOpenDaw = juce::MidiInput::openDevice(identifierOf(juce::MidiInput::getAvailableDevices(),
                                                                VirtualMidiPorts::kOutName), &listener);
    EXPECT(fromOpenDaw != nullptr);
    if (fromOpenDaw) fromOpenDaw->start();

    auto* outTrack = editMgr.addMidiTrack();
    auto* out = dynamic_cast<MidiOutPlugin*>(editMgr.setTrackMidiOut(*outTrack));
    EXPECT(out != nullptr);
    EXPECT(out && out->destination() == VirtualMidiPorts::kVirtualDestination);
    EXPECT(out && out->problem().isEmpty());
    EXPECT(out && editMgr.getTrackInstrument(outTrack) == out);
    if (out) {
        // A note on at the block start and its note off 40 ms in: they leave
        // in order, about 40 ms apart, not both at once.
        te::MidiMessageArray midi;
        midi.addMidiMessage(juce::MidiMessage::noteOn(3, 64, juce::uint8(90)), 0.0, te::MPESourceID{});
        midi.addMidiMessage(juce::MidiMessage::noteOff(3, 64), 0.040, te::MPESourceID{});
        renderBlock(*out, midi);
        EXPECT(midi.isEmpty());   // nothing left for the track's audio chain
        EXPECT(waitFor([&] { return listener.snapshot().size() >= 2; }));
        auto got = listener.snapshot();
        if (got.size() >= 2) {
            EXPECT(got[0].message.isNoteOn() && got[0].message.getNoteNumber() == 64 && got[0].message.getChannel() == 3);
            EXPECT(got[1].message.isNoteOff() && got[1].message.getNoteNumber() == 64);
            const double gap = got[1].atMs - got[0].atMs;
            std::printf("  note on -> off: %.1f ms (40 expected)\n", gap);
            EXPECT(gap > 30.0 && gap < 60.0);
        }

        // Channel 0 keeps channels; 1..16 sends everything on one, as one undo step.
        listener.clear();
        um.clearUndoHistory();
        out->setChannel(10, "Change MIDI Out Channel");
        EXPECT(out->channel() == 10);
        midi.addMidiMessage(juce::MidiMessage::noteOn(1, 36, juce::uint8(100)), 0.0, te::MPESourceID{});
        renderBlock(*out, midi);
        EXPECT(waitFor([&] { return !listener.snapshot().empty(); }));
        EXPECT(!listener.snapshot().empty() && listener.snapshot()[0].message.getChannel() == 10);
        um.undo();
        EXPECT(out->channel() == 0);

        // Stop / jump: everything held on the other side is released.
        listener.clear();
        midi.isAllNotesOff = true;
        renderBlock(*out, midi);
        EXPECT(waitFor([&] { return listener.snapshot().size() >= 32; }));
        int allOff = 0;
        for (const auto& g : listener.snapshot()) allOff += g.message.isAllNotesOff() ? 1 : 0;
        EXPECT(allOff == 16);

        // A destination that isn't there: nothing is sent, and the dialog says why.
        listener.clear();
        out->setDestination("Unplugged Synth", "Change MIDI Out Destination");
        EXPECT(out->problem().contains("isn't connected"));
        midi.addMidiMessage(juce::MidiMessage::noteOn(1, 40, juce::uint8(100)), 0.0, te::MPESourceID{});
        renderBlock(*out, midi);
        settle(100);
        EXPECT(listener.snapshot().empty());
        EXPECT(out->pendingEvents() == 0);   // dropped, not piling up
        {
            MidiOutDialog dialog(*out);
            EXPECT(dialog.destinationBox()->currentText().contains("not connected"));
            EXPECT(dialog.statusText().contains("isn't connected"));
            // Choosing the virtual port in the dialog is one undo step.
            const int virtualIndex = dialog.destinationBox()->findData(QString(VirtualMidiPorts::kVirtualDestination));
            EXPECT(virtualIndex >= 0);
            um.clearUndoHistory();
            dialog.destinationBox()->setCurrentIndex(virtualIndex);
            emit dialog.destinationBox()->activated(virtualIndex);
            EXPECT(out->destination() == VirtualMidiPorts::kVirtualDestination);
            EXPECT(um.canUndo());
            EXPECT(dialog.statusText().startsWith("Sending to"));
        }
        um.undo();
        EXPECT(out->destination() == "Unplugged Synth");
        um.redo();

        // Saved with the project.
        auto copy = editMgr.edit()->getPluginCache().createNewPlugin(out->state.createCopy());
        auto* restored = dynamic_cast<MidiOutPlugin*>(copy.get());
        EXPECT(restored && restored->destination() == VirtualMidiPorts::kVirtualDestination);
    }

    // ── MIDI clock and transport follow OpenDaw ──
    std::printf("midi clock\n");
    {
        // The edit has one, hidden first on the master.
        auto& master = editMgr.edit()->getMasterPluginList();
        EXPECT(master.getPluginsOfType<MidiClockPlugin>().size() == 1);
        EXPECT(dynamic_cast<MidiClockPlugin*>(master[0]) != nullptr);
        editMgr.ensureMidiClock();
        EXPECT(master.getPluginsOfType<MidiClockPlugin>().size() == 1);   // never twice

        // Its own instance, outside the running graph, fed blocks in real time.
        auto made = editMgr.edit()->getPluginCache().createNewPlugin(juce::String(MidiClockPlugin::xmlTypeName), {});
        auto* clock = dynamic_cast<MidiClockPlugin*>(made.get());
        EXPECT(clock != nullptr);
        if (clock) {
            clock->initialise({ tracktion::TimePosition(), 44100.0, 512 });
            const double block = 512.0 / 44100.0;
            auto play = [&](double from, double seconds, bool playing) {
                for (double t = from; t < from + seconds - 1.0e-9; t += block) {
                    juce::AudioBuffer<float> buffer(2, 512);
                    buffer.clear();
                    te::PluginRenderContext rc(&buffer, juce::AudioChannelSet::stereo(), 0, 512, nullptr, 0.0,
                                               tracktion::TimeRange(tracktion::TimePosition::fromSeconds(t),
                                                             tracktion::TimePosition::fromSeconds(t + block)),
                                               playing, false, false, false);
                    clock->applyToBuffer(rc);
                    juce::Thread::sleep(int(block * 1000.0));
                }
            };
            auto count = [](const std::vector<Listener::Got>& got, auto pred) {
                int n = 0;
                for (const auto& g : got) n += pred(g.message) ? 1 : 0;
                return n;
            };
            auto isClock = [](const juce::MidiMessage& m) { return m.isMidiClock(); };

            // Play one second from the top at 120 BPM: position 0, start, 48 ticks.
            listener.clear();
            play(0.0, 1.0, true);
            settle(100);
            auto got = listener.snapshot();
            EXPECT(got.size() >= 2 && got[0].message.isSongPositionPointer()
                   && got[0].message.getSongPositionPointerMidiBeat() == 0);
            EXPECT(got.size() >= 2 && got[1].message.isMidiStart());
            const int ticks = count(got, isClock);
            std::printf("  %d clock ticks in 1 s at 120 BPM (48 expected)\n", ticks);
            EXPECT(ticks >= 47 && ticks <= 49);
            std::vector<double> times;
            for (const auto& g : got) if (g.message.isMidiClock()) times.push_back(g.atMs);
            if (times.size() > 10) {
                const double mean = (times.back() - times.front()) / double(times.size() - 1);
                std::printf("  tick spacing %.2f ms (20.83 expected)\n", mean);
                EXPECT(mean > 18.0 && mean < 24.0);
            }

            // Stop sends stop; playing again after a jump sends position and continue.
            listener.clear();
            play(1.0, 0.05, false);
            settle(50);
            EXPECT(count(listener.snapshot(), [](const juce::MidiMessage& m) { return m.isMidiStop(); }) == 1);
            EXPECT(count(listener.snapshot(), isClock) == 0);
            listener.clear();
            play(10.0, 0.6, true);   // 10 s = beat 20 = sixteenth 80
            settle(100);
            got = listener.snapshot();
            EXPECT(!got.empty() && got[0].message.isSongPositionPointer()
                   && got[0].message.getSongPositionPointerMidiBeat() == 80);
            EXPECT(got.size() > 1 && got[1].message.isMidiContinue());

            // A loop back sends the position again.
            listener.clear();
            play(2.0, 0.3, true);
            settle(100);
            got = listener.snapshot();
            EXPECT(!got.empty() && got[0].message.isSongPositionPointer()
                   && got[0].message.getSongPositionPointerMidiBeat() == 16);

            // A short loop (less than the jump threshold) also resends it.
            play(2.0, 0.1, true);
            listener.clear();
            play(2.0, 0.1, true);
            settle(100);
            got = listener.snapshot();
            EXPECT(!got.empty() && got[0].message.isSongPositionPointer()
                   && got[0].message.getSongPositionPointerMidiBeat() == 16);

            // Switched off in Settings: nothing at all.
            ports.setClockEnabled(false);
            listener.clear();
            play(3.0, 0.3, true);
            play(3.3, 0.05, false);
            settle(100);
            EXPECT(listener.snapshot().empty());
            ports.setClockEnabled(true);
            EXPECT(QSettings().value(VirtualMidiPorts::kClockKey).toBool());
        }
    }

    // ── Switching the ports off withdraws them ──
    std::printf("ports off\n");
    ports.setEnabled(false);
    EXPECT(!ports.isEnabled());
    EXPECT(waitFor([] { return !hasDevice(juce::MidiOutput::getAvailableDevices(), VirtualMidiPorts::kInName); }));
    EXPECT(out && out->problem().contains("switched off"));
    ports.setEnabled(true);
    EXPECT(ports.isEnabled());

    if (fromOpenDaw) fromOpenDaw->stop();
    std::printf(failures == 0 ? "\nAll MIDI port checks passed.\n" : "\n%d check(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
