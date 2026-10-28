// Offscreen checks of the Modular patch editor against a real Edit and
// Modular Synth: the scene matches the patch, mouse gestures go through
// applyOps/setPatch as exactly one undo step, and outside changes (AI,
// undo) rebuild the view. Run with QT_QPA_PLATFORM=offscreen.

#include <cmath>
#include "app/JuceQtBridge.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/HumRecorder.h"
#include "engine/MelodyToMidi.h"
#include "ai/AiToolExecutor.h"
#include "engine/PluginScanner.h"
#include "drums/SampleLibrary.h"
#include "engine/drums/DrumMachinePlugin.h"
#include "engine/modular/ModularSynthPlugin.h"
#include "ui/controls/RotaryKnob.h"
#include "modular/FaustStarters.h"
#include "ui/modular/FaustCodeDialog.h"
#include "ui/modular/ModularEditorView.h"
#include "ui/mixer/MixerView.h"
#include "ui/timeline/TimelineView.h"
#include "ui/modular/ModulePanelItem.h"

#include <QApplication>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <QElapsedTimer>
#include <QGraphicsView>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <cstdio>

using namespace OpenDaw;
namespace M = OpenDaw::Modular;

namespace {

int failures = 0;

#define EXPECT(cond)                                                              \
    do {                                                                          \
        if (cond) std::printf("  ok    %s\n", #cond);                             \
        else { std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__); ++failures; } \
    } while (0)

void settle(int ms = 50)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
#if JUCE_MAC
        // Offscreen Qt doesn't run the native loop JUCE's timers post to.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
#endif
    }
}

void send(QWidget* target, QEvent::Type type, QPoint pos, Qt::MouseButton button,
          Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(target->mapToGlobal(pos)), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(target, &ev);
}

// Number of undo steps: undoes until nothing is left, then redoes them all.
int countUndoSteps(juce::UndoManager& um)
{
    int n = 0;
    while (um.canUndo()) { um.undo(); ++n; }
    for (int i = 0; i < n; ++i) um.redo();
    return n;
}

bool hasCable(const M::Patch& p, const char* from, const char* to)
{
    return p.cables.contains(M::Cable{ *M::parsePortRef(from), *M::parsePortRef(to) });
}

} // namespace

// 0.1 s of a 440 Hz sine, stereo, 22.05 kHz.
void writeTestWav(const juce::File& wav)
{
    wav.deleteFile();
    wav.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(wav.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), 22050.0, 2, 16, {}, 0));
    if (!writer) return;
    stream.release();   // the writer owns it now
    juce::AudioBuffer<float> buffer(2, 2205);
    for (int i = 0; i < 2205; ++i)
        buffer.setSample(0, i, 0.5f * std::sin(6.283185307 * 440.0 * i / 22050.0)),
        buffer.setSample(1, i, buffer.getSample(0, i));
    writer->writeFromAudioSampleBuffer(buffer, 0, 2205);
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Own settings, so the user's samples folder is never touched.
    QCoreApplication::setOrganizationName("OpenDawTests");
    QCoreApplication::setApplicationName("ModularEditorTest");
    juce::ScopedJuceInitialiser_GUI juceInit;

    JuceQtBridge bridge;
    bridge.start();
    AudioEngine engine;
    EditManager editMgr(engine);
    editMgr.stopAutosave();

    auto* track = editMgr.addMidiTrack();
    auto* synth = track ? dynamic_cast<ModularSynthPlugin*>(editMgr.setTrackModularSynth(*track)) : nullptr;
    if (!synth) {
        std::printf("FAIL: could not create a Modular Synth\n");
        return 1;
    }
    synth->setPatch(M::Patch::initPatch(), {});
    auto& um = editMgr.edit()->getUndoManager();

    ModularEditorView view(&editMgr);
    view.resize(1400, 900);
    view.show();
    view.setTrack(track);
    settle();

    const auto init = M::Patch::initPatch();
    std::printf("init patch in the view\n");
    EXPECT(view.synth() == synth);
    EXPECT(view.moduleItemCount() == int(init.modules.size()));
    EXPECT(view.moduleItemCount() == 8);
    EXPECT(view.cableItemCount() == int(init.cables.size()));
    EXPECT(view.cableItemCount() == 9);
    EXPECT(view.panel("vcf1") && view.panel("vcf1")->knob("cutoff"));

    // ── Connect by dragging adsr1.out onto vco1.fm, as a user would ──
    std::printf("connect by mouse drag\n");
    um.clearUndoHistory();
    auto* gv = view.findChild<QGraphicsView*>();
    auto* viewport = gv->viewport();
    auto* from = view.panel("adsr1")->findJack("out", false);
    auto* to = view.panel("vco1")->findJack("fm", true);
    const QPoint a = gv->mapFromScene(from->sceneCenterPos());
    const QPoint b = gv->mapFromScene(to->sceneCenterPos());
    send(viewport, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    send(viewport, QEvent::MouseMove, (a + b) / 2, Qt::NoButton, Qt::LeftButton);
    send(viewport, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    settle();
    EXPECT(hasCable(synth->patch(), "adsr1.out", "vco1.fm"));
    EXPECT(view.cableItemCount() == 10);
    EXPECT(countUndoSteps(um) == 1);
    EXPECT(um.getUndoDescription() == "Connect Cable");
    um.undo();
    settle();
    EXPECT(!hasCable(synth->patch(), "adsr1.out", "vco1.fm"));
    EXPECT(view.cableItemCount() == 9);   // undo rebuilt the view

    // ── An invalid connection changes nothing and reports why ──
    std::printf("invalid connection\n");
    um.clearUndoHistory();
    EXPECT(!view.connectPorts({ "vco1", "out" }, { "output", "left" }));  // voice -> global
    EXPECT(!view.lastError().isEmpty());
    EXPECT(!um.canUndo());
    EXPECT(synth->patch().cables.size() == init.cables.size());

    // ── A knob drag is one undo step and touches the patch only on release ──
    std::printf("knob gesture\n");
    um.clearUndoHistory();
    auto* knob = view.panel("vcf1")->knob("cutoff");
    const float before = synth->patch().paramValue(*synth->patch().find("vcf1"),
        M::findModule("vcf")->paramIndex("cutoff"));
    const QPoint c = knob->rect().center();
    send(knob, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    for (int dy = 4; dy <= 40; dy += 4)
        send(knob, QEvent::MouseMove, c - QPoint(0, dy), Qt::NoButton, Qt::LeftButton);
    settle();
    EXPECT(!um.canUndo());   // nothing stored while dragging
    send(knob, QEvent::MouseButtonRelease, c - QPoint(0, 40), Qt::LeftButton, Qt::NoButton);
    settle();
    const auto afterPatch = synth->patch();
    const float after = afterPatch.paramValue(*afterPatch.find("vcf1"),
        M::findModule("vcf")->paramIndex("cutoff"));
    std::printf("  cutoff %.1f -> %.1f\n", double(before), double(after));
    EXPECT(after > before);
    EXPECT(countUndoSteps(um) == 1);
    EXPECT(um.getUndoDescription() == "Change Parameter");

    // ── An outside edit (as the AI makes) rebuilds the view ──
    std::printf("live sync\n");
    auto patch = synth->patch();
    EXPECT(M::applyOps(patch, { QJsonObject{ { "op", "add_module" }, { "id", "lfo9" },
                                             { "type", "lfo" } } }).isEmpty());
    synth->setPatch(patch, "AI: Edit Modular Patch");
    settle();
    EXPECT(view.moduleItemCount() == 9);
    EXPECT(view.panel("lfo9") != nullptr);

    // ── Pulling a cable off its input disconnects it ──
    std::printf("disconnect by dragging a cable end off\n");
    um.clearUndoHistory();
    auto* plugged = view.panel("vcf1")->findJack("in", true);
    const QPoint p = gv->mapFromScene(plugged->sceneCenterPos());
    send(viewport, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    send(viewport, QEvent::MouseMove, p + QPoint(0, 120), Qt::NoButton, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, p + QPoint(0, 120), Qt::LeftButton, Qt::NoButton);
    settle();
    EXPECT(!hasCable(synth->patch(), "vco1.out", "vcf1.in"));
    EXPECT(countUndoSteps(um) == 1);
    EXPECT(um.getUndoDescription() == "Disconnect Cable");

    // ── Zoom survives rebuilds and switching tracks ──
    std::printf("zoom is kept\n");
    view.zoomToFit();
    settle();
    const double fitted = view.zoomLevel();
    view.zoomOut();
    view.zoomOut();
    const double zoomed = view.zoomLevel();
    EXPECT(std::abs(zoomed - fitted / 1.5625) < 1e-6);
    view.scheduleRebuild();
    settle();
    EXPECT(std::abs(view.zoomLevel() - zoomed) < 1e-6);

    auto* other = editMgr.addMidiTrack();
    EXPECT(other && editMgr.setTrackModularSynth(*other));
    view.setTrack(other);
    settle();
    view.setTrack(track);
    settle();
    EXPECT(view.synth() == synth);
    EXPECT(std::abs(view.zoomLevel() - zoomed) < 1e-6);

    // ── A warning's Fix button repairs the patch in one undo step ──
    std::printf("warning fix button\n");
    {
        auto patch = M::Patch::initPatch();
        EXPECT(M::applyOps(patch, {
            QJsonObject{ { "op", "add_module" }, { "id", "hiss" }, { "type", "noise" }, { "section", "global" } },
            QJsonObject{ { "op", "connect" }, { "from", "hiss.out" }, { "to", "voice_out.left" } },
        }).isEmpty());
        synth->setPatch(patch, {});
        settle();
        um.clearUndoHistory();
        EXPECT(view.issues().size() == 1 && !view.issues()[0].fixOps.isEmpty());
        QPushButton* fixButton = nullptr;
        for (auto* b : view.findChildren<QPushButton*>())
            if (b->accessibleName().startsWith("Fix: ") && b->isVisibleTo(&view)) fixButton = b;
        EXPECT(fixButton != nullptr);
        if (fixButton) fixButton->click();
        settle();
        EXPECT(synth->patch().warnings().isEmpty());
        EXPECT(view.issues().isEmpty());
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription().startsWith("Fix: "));
        um.undo();
        settle();
        EXPECT(view.issues().size() == 1);   // undo brings the warning back
    }

    // ── A code module shows its compiled ports, knobs and cables ──
    if (M::codeModulesAvailable()) {
        std::printf("code module\n");
        auto coded = M::Patch::initPatch();
        const QString code = "import(\"stdfaust.lib\");\n"
                             "mix = hslider(\"mix\", 0.5, 0, 1, 0.01);\n"
                             "process = *(mix);";
        EXPECT(M::applyOps(coded, {
            QJsonObject{ { "op", "disconnect" }, { "from", "vcf1.out" }, { "to", "vca1.in" } },
            QJsonObject{ { "op", "write_module" }, { "id", "crusher" }, { "code", code } },
            QJsonObject{ { "op", "connect" }, { "from", "vcf1.out" }, { "to", "crusher.in" } },
            QJsonObject{ { "op", "connect" }, { "from", "crusher.out" }, { "to", "vca1.in" } },
        }).isEmpty());
        synth->setPatch(coded, "AI: Edit Modular Patch");
        settle();
        auto* crusher = view.panel("crusher");
        EXPECT(crusher != nullptr);
        EXPECT(crusher && crusher->findJack("in", true) && crusher->findJack("out", false));
        EXPECT(crusher && crusher->knob("mix"));
        EXPECT(view.cableItemCount() == int(coded.cables.size()));
    }

    // ── Writing code modules by hand, as the code dialog does ──
    if (M::codeModulesAvailable()) {
        std::printf("code modules by hand\n");
        synth->setPatch(M::Patch::initPatch(), {});
        settle();
        const auto tmpl = M::compileCodeModule("tmpl", FaustCodeDialog::templateCode().toStdString());
        EXPECT(tmpl.def && tmpl.def->inputs.size() == 1 && tmpl.def->outputs.size() == 1
               && tmpl.def->params.size() == 2);

        um.clearUndoHistory();
        const QPointF voiceZone = view.panel("vco1")->scenePos() + QPointF(0, 260);
        EXPECT(view.addCodeModule(FaustCodeDialog::templateCode(), voiceZone));
        settle();
        auto* code = view.panel("code1");
        EXPECT(code && code->knob("gain") && code->knob("drive") && code->codeError().isEmpty());
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Add Code Module");

        // A starter from the menu is named after itself.
        const auto& crusher = M::codeStarters().front();
        EXPECT(view.addCodeModule(crusher.code, voiceZone + QPointF(240, 0), crusher.name));
        settle();
        EXPECT(view.panel(QString(crusher.name) + "1") != nullptr);

        EXPECT(view.connectPorts({ "vco1", "out" }, { "code1", "in" }));
        settle();
        um.clearUndoHistory();
        const QString twoKnobs = "import(\"stdfaust.lib\");\n"
                                 "gain = hslider(\"gain\", 1, 0, 4, 0.01);\n"
                                 "tone = hslider(\"tone\", 0.5, 0, 1, 0.01);\n"
                                 "process = *(gain) : fi.lowpass(1, 200 + tone * 8000);";
        EXPECT(view.editModuleCode("code1", twoKnobs));
        settle();
        code = view.panel("code1");
        EXPECT(code && code->knob("tone") && code->knob("gain") && !code->knob("drive"));
        EXPECT(hasCable(synth->patch(), "vco1.out", "code1.in"));   // its port still exists
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Edit Code");

        // Code without an input would drop the vco1 cable: refused unless allowed.
        um.clearUndoHistory();
        const QString noInput = "import(\"stdfaust.lib\");\nprocess = os.osc(220);";
        const auto before = synth->patch().toJson();
        EXPECT(view.cablesDroppedBy("code1", noInput).size() == 1);
        EXPECT(!view.editModuleCode("code1", noInput));
        EXPECT(!view.lastError().isEmpty());
        EXPECT(synth->patch().toJson() == before);
        EXPECT(!um.canUndo());

        // A code module without a compiled definition (code from a Faust build
        // opened in one without Faust) is marked as an error, not an empty box.
        M::PatchModule uncompiled;
        uncompiled.id = "code9";
        uncompiled.type = M::kCodeModuleType;
        uncompiled.code = "process = _;";
        const M::Patch empty;
        ModulePanelItem marked(uncompiled, *M::moduleDef(uncompiled), empty);
        EXPECT(!marked.codeError().isEmpty());
        EXPECT(view.panel("code1")->codeError().isEmpty());
    }

    // ── Losing the instrument shows the empty state ──
    std::printf("empty state\n");
    editMgr.removeTrackInstrument(*track);
    emit editMgr.tracksChanged();
    settle();
    EXPECT(view.synth() == nullptr);
    EXPECT(view.moduleItemCount() == 0);

    // ── The Drum Machine instrument (not the editor, but this is the app harness) ──
    std::printf("drum machine\n");
    {
        auto* drumTrack = editMgr.addMidiTrack();
        auto* drums = drumTrack ? dynamic_cast<DrumMachinePlugin*>(editMgr.setTrackDrumMachine(*drumTrack)) : nullptr;
        EXPECT(drums != nullptr);
        if (drums) {
            EXPECT(editMgr.getTrackInstrument(drumTrack) == drums);
            EXPECT(drums->kit().toJson() == Drums::Kit::defaultKit().toJson());
            EXPECT(drums->warnings().isEmpty());

            um.clearUndoHistory();
            auto kit = drums->kit();
            kit.padForNote(Drums::Gm::Snare)->level = 0.3f;
            kit.padForNote(Drums::Gm::Kick)->pan = -0.5f;
            drums->setKit(kit, "Change Pad");
            EXPECT(drums->kit().padForNote(Drums::Gm::Snare)->level == 0.3f);
            EXPECT(countUndoSteps(um) == 1);
            um.undo();
            EXPECT(drums->kit().padForNote(Drums::Gm::Snare)->level == 0.8f);
            um.redo();
            EXPECT(drums->kit().padForNote(Drums::Gm::Kick)->pan == -0.5f);

            // Saved state restores into a fresh plugin (as reopening a project does).
            auto fresh = editMgr.edit()->getPluginCache().createNewPlugin(
                juce::String(DrumMachinePlugin::xmlTypeName), {});
            auto* copy = dynamic_cast<DrumMachinePlugin*>(fresh.get());
            EXPECT(copy != nullptr);
            if (copy) {
                copy->restorePluginStateFromValueTree(drums->state);
                EXPECT(copy->kit().toJson() == drums->kit().toJson());
            }

            // A sample pad whose file is missing is reported, not fatal.
            kit = drums->kit();
            kit.padForNote(Drums::Gm::Clap)->source = Drums::PadSource::Sample;
            kit.padForNote(Drums::Gm::Clap)->sample = "/nonexistent/clap.wav";
            drums->setKit(kit, "Load Sample");
            EXPECT(drums->warnings().size() == 1 && drums->warnings()[0].contains("clap.wav"));

            // A real WAV file loads (written here, 0.1 s of a 440 Hz sine, stereo, 22.05 kHz).
            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("opendaw_drum_test.wav");
            writeTestWav(wav);
            kit.padForNote(Drums::Gm::Clap)->sample = QString::fromStdString(wav.getFullPathName().toStdString());
            drums->setKit(kit, "Load Sample");
            EXPECT(wav.existsAsFile());
            EXPECT(drums->warnings().isEmpty());
            wav.deleteFile();
        }
    }

    // ── The Drum Machine AI tools, through the real tool executor ──
    std::printf("drum AI tools\n");
    {
        PluginScanner scanner(engine);
        AiToolExecutor tools(&editMgr, &engine, &scanner);
        auto call = [&](const char* name, QJsonObject input) {
            const auto r = tools.execute(AiToolCall{ "t1", name, input });
            if (r.isError) std::printf("    (%s error: %s)\n", name, r.content.toUtf8().constData());
            return std::make_pair(!r.isError, QJsonDocument::fromJson(r.content.toUtf8()).object());
        };
        auto* drumTrack = editMgr.addMidiTrack();
        drumTrack->setName("Groove");
        const int clipsBefore = drumTrack->getClips().size();

        // An unknown style changes nothing and names the valid ones.
        um.clearUndoHistory();
        const auto r0 = tools.execute(AiToolCall{ "t0", "write_drum_pattern",
                                                  QJsonObject{ { "track", "Groove" }, { "style", "polka" } } });
        EXPECT(r0.isError && r0.content.contains("blues_shuffle"));
        EXPECT(editMgr.getTrackInstrument(drumTrack) == nullptr && !um.canUndo());

        // A groove on a track without an instrument loads the Drum Machine with it.
        auto [ok1, r1] = call("write_drum_pattern", QJsonObject{ { "track", "Groove" }, { "style", "blues_shuffle" },
                                                                 { "bars", 8 }, { "fill_every", 4 } });
        EXPECT(ok1 && r1["drum_machine_created"].toBool());
        auto* drums = dynamic_cast<DrumMachinePlugin*>(editMgr.getTrackInstrument(drumTrack));
        EXPECT(drums != nullptr);
        EXPECT(drumTrack->getClips().size() == clipsBefore + 1);
        auto* clip = dynamic_cast<te::MidiClip*>(drumTrack->getClips().getLast());
        EXPECT(clip && clip->getMidiChannel().getChannelNumber() == 10);
        EXPECT(clip && clip->getSequence().getNumNotes() == r1["added"].toInt() && r1["added"].toInt() > 50);
        EXPECT(r1["length_beats"].toDouble() == 32.0 && !r1.contains("missing_pads"));
        EXPECT(countUndoSteps(um) == 1);
        um.undo();
        EXPECT(drumTrack->getClips().size() == clipsBefore);
        EXPECT(editMgr.getTrackInstrument(drumTrack) == nullptr);   // one step undoes both
        um.redo();
        drums = dynamic_cast<DrumMachinePlugin*>(editMgr.getTrackInstrument(drumTrack));
        clip = dynamic_cast<te::MidiClip*>(drumTrack->getClips().getLast());

        // Writing into the clip again replaces the notes in its range.
        auto [ok2, r2] = call("write_drum_pattern", QJsonObject{ { "track", "Groove" }, { "style", "rock" },
                                                                 { "bars", 8 }, { "clip_index", clipsBefore } });
        EXPECT(ok2 && clip && clip->getSequence().getNumNotes() == r2["added"].toInt());
        EXPECT(!r2.contains("drum_machine_created"));

        // Kit edits: tune a tom, add a pad, remove it, and refuse bad input.
        auto [ok3, r3] = call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 45 },
                                                           { "params", QJsonObject{ { "tune", 100 } } } });
        EXPECT(ok3 && drums && drums->kit().padForNote(45)->params.value("tune") == 100.0f);
        EXPECT(drums && drums->kit().padForNote(45)->module == "tom");   // other fields kept
        auto [ok4, r4] = call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 75 }, { "module", "rimshot" } });
        EXPECT(ok4 && drums && drums->kit().padForNote(75) && drums->kit().padForNote(75)->name == "Claves");
        auto [ok5, r5] = call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 75 }, { "remove", true } });
        EXPECT(ok5 && drums && !drums->kit().padForNote(75));
        const auto kitBefore = drums ? drums->kit().toJson() : QJsonObject();
        EXPECT(!call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 38 }, { "module", "vco" } }).first);
        EXPECT(!call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 38 }, { "source", "sample" },
                                                  { "sample", "/nonexistent/snare.wav" } }).first);
        EXPECT(drums && drums->kit().toJson() == kitBefore);

        auto [ok6, r6] = call("get_drum_kit", QJsonObject{ { "track", "Groove" } });
        bool hasTom = false;
        for (const auto& v : r6["synth_voices"].toArray()) hasTom |= v.toObject()["type"] == "tom";
        EXPECT(ok6 && hasTom && drums && r6["kit"].toObject()["pads"].toArray().size() == int(drums->kit().pads.size()));
        auto [ok7, r7] = call("list_drum_styles", {});
        EXPECT(ok7 && r7["styles"].toArray().size() >= 15);

        // Samples are named relative to the samples folder, which get_drum_kit reports;
        // an absolute path inside it is stored relative too.
        QTemporaryDir samples;
        Drums::setSamplesFolder(samples.path());
        writeTestWav(juce::File(juce::String((samples.path() + "/Acoustic/snare.wav").toStdString())));
        EXPECT(call("get_drum_kit", QJsonObject{ { "track", "Groove" } }).second["samples_folder"] == samples.path());
        auto [ok9, r9] = call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 38 }, { "source", "sample" },
                                                           { "sample", "Acoustic/snare.wav" } });
        EXPECT(ok9 && drums && drums->kit().padForNote(38)->sample == "Acoustic/snare.wav");
        EXPECT(drums && drums->warnings().isEmpty());
        auto [ok10, r10] = call("set_drum_pad", QJsonObject{ { "track", "Groove" }, { "note", 40 }, { "source", "sample" },
                                                             { "sample", samples.path() + "/Acoustic/snare.wav" } });
        EXPECT(ok10 && drums && drums->kit().padForNote(40)->sample == "Acoustic/snare.wav");
        const auto missing = tools.execute(AiToolCall{ "t9", "set_drum_pad",
            QJsonObject{ { "track", "Groove" }, { "note", 41 }, { "source", "sample" }, { "sample", "Acoustic/nope.wav" } } });
        EXPECT(missing.isError && missing.content.contains(samples.path()));

        // Kit presets: save this kit, load a built-in, load it back; each load is one step.
        auto [okL, rL] = call("list_drum_kits", {});
        EXPECT(okL && rL["presets"].toArray().size() == 3 && rL["presets"].toArray()[1].toObject()["name"] == "Synth 808");
        auto [okS, rS] = call("save_drum_kit", QJsonObject{ { "track", "Groove" }, { "name", "Groove Kit" } });
        EXPECT(okS && rS["saved"] == "Groove Kit" && !rS.contains("outside_samples_folder"));
        EXPECT(!call("save_drum_kit", QJsonObject{ { "track", "Groove" }, { "name", "groove kit" } }).first);
        EXPECT(call("list_drum_kits", {}).second["presets"].toArray().size() == 4);
        const auto savedKit = drums ? drums->kit().toJson() : QJsonObject();
        um.clearUndoHistory();
        auto [okB, rB] = call("load_drum_kit", QJsonObject{ { "track", "Groove" }, { "name", "synth 909" } });
        EXPECT(okB && rB["loaded"] == "Synth 909" && !rB.contains("drum_machine_created"));
        EXPECT(drums && drums->kit().padForNote(38)->source == Drums::PadSource::Synth);
        EXPECT(countUndoSteps(um) == 1);
        auto [okG, rG] = call("load_drum_kit", QJsonObject{ { "track", "Groove" }, { "name", "Groove Kit" } });
        EXPECT(okG && drums && drums->kit().toJson() == savedKit && drums->warnings().isEmpty());
        um.undo();
        EXPECT(drums && drums->kit().padForNote(38)->source == Drums::PadSource::Synth);   // back to the 909
        const auto unknown = tools.execute(AiToolCall{ "tk", "load_drum_kit",
            QJsonObject{ { "track", "Groove" }, { "name", "Jazz Brushes" } } });
        EXPECT(unknown.isError && unknown.content.contains("Synth 808") && unknown.content.contains("Groove Kit"));

        // Sample listing and kits built from a folder.
        for (const char* f : { "Acoustic/Kick.wav", "Acoustic/Snare Ghost.wav", "Acoustic/Toms/Floor.wav",
                               "Acoustic/Room.wav", "808/BD.wav" })
            writeTestWav(juce::File(juce::String((samples.path() + "/" + f).toStdString())));
        auto [okLs, rLs] = call("list_drum_samples", QJsonObject{ { "filter", "acoustic/" } });
        EXPECT(okLs && rLs["total"].toInt() == 5);   // snare.wav from before, and the four above
        bool kickGuessed = false;
        for (const auto& v : rLs["files"].toArray())
            kickGuessed |= v.toObject()["path"] == "Acoustic/Kick.wav" && v.toObject()["guess"].toString().contains("36");
        EXPECT(kickGuessed);
        EXPECT(call("list_drum_samples", {}).second["subfolders"].toArray().size() == 2);
        EXPECT(!call("list_drum_samples", QJsonObject{ { "folder", "Nope" } }).first);

        um.clearUndoHistory();
        auto [okF, rF] = call("build_drum_kit_from_folder", QJsonObject{ { "track", "Groove" }, { "folder", "Acoustic" },
                                                                         { "save_as", "Acoustic" } });
        EXPECT(okF && drums && drums->kit().padForNote(36)->sample == "Acoustic/Kick.wav");
        EXPECT(drums && drums->kit().padForNote(41)->sample == "Acoustic/Toms/Floor.wav");
        EXPECT(drums && drums->kit().padForNote(38)->sample == "Acoustic/snare.wav");   // not the ghost
        EXPECT(rF["unmatched"].toArray().size() == 1 && rF["unmatched"].toArray()[0] == "Acoustic/Room.wav");
        EXPECT(rF["saved_preset"] == "Acoustic");
        EXPECT(countUndoSteps(um) == 1);
        bool ghostOffered = false;
        for (const auto& v : rF["mapped"].toArray())
            if (v.toObject()["note"].toInt() == 38)
                ghostOffered = v.toObject()["other_candidates"].toArray().contains("Acoustic/Snare Ghost.wav");
        EXPECT(ghostOffered);
        const auto noFolder = tools.execute(AiToolCall{ "tf", "build_drum_kit_from_folder",
                                                        QJsonObject{ { "track", "Groove" }, { "folder", "Nowhere" } } });
        EXPECT(noFolder.isError && !um.canRedo() && countUndoSteps(um) == 1);

        // On a track without an instrument it loads the Drum Machine too, in the same step.
        auto* kitTrack = editMgr.addMidiTrack();
        kitTrack->setName("Kit Track");
        um.clearUndoHistory();
        auto [okN, rN] = call("load_drum_kit", QJsonObject{ { "track", "Kit Track" }, { "name", "Synth 808" } });
        EXPECT(okN && rN["drum_machine_created"].toBool());
        EXPECT(dynamic_cast<DrumMachinePlugin*>(editMgr.getTrackInstrument(kitTrack)) != nullptr);
        EXPECT(countUndoSteps(um) == 1);
        um.undo();
        EXPECT(editMgr.getTrackInstrument(kitTrack) == nullptr);
        um.clearUndoHistory();
        auto [okNF, rNF] = call("build_drum_kit_from_folder", QJsonObject{ { "track", "Kit Track" }, { "folder", "808" } });
        EXPECT(okNF && rNF["drum_machine_created"].toBool() && countUndoSteps(um) == 1);
        um.undo();
        EXPECT(editMgr.getTrackInstrument(kitTrack) == nullptr);

        // A track with another instrument keeps it, and the result says so.
        auto* synthTrack = editMgr.addMidiTrack();
        synthTrack->setName("Keys");
        editMgr.setTrackModularSynth(*synthTrack);
        auto [ok8, r8] = call("write_drum_pattern", QJsonObject{ { "track", "Keys" }, { "style", "funk" } });
        EXPECT(ok8 && r8["note"].toString().contains("create_drum_machine"));
        EXPECT(dynamic_cast<ModularSynthPlugin*>(editMgr.getTrackInstrument(synthTrack)) != nullptr);
    }

    // ── A hummed melody in an audio clip becomes MIDI (transcribe_melody) ──
    std::printf("melody to MIDI\n");
    {
        PluginScanner scanner(engine);
        AiToolExecutor tools(&editMgr, &engine, &scanner);
        // A hum, 0.5 s per note with a "t" gap: E G A G, at 120 BPM = one beat each.
        const int pitches[] = { 64, 67, 69, 67 };
        const double sr = 44100.0;
        juce::AudioBuffer<float> hum(1, int(sr * 2.5));
        hum.clear();
        double phase = 0.0;
        for (int k = 0; k < 4; ++k) {
            const double hz = 440.0 * std::pow(2.0, (pitches[k] - 69) / 12.0);
            const int a = int((0.25 + 0.5 * k) * sr), b = a + int(0.42 * sr);
            for (int i = a; i < b; ++i) {
                phase += 2.0 * juce::MathConstants<double>::pi * hz / sr;
                const double env = std::min({ 1.0, (i - a) / (0.01 * sr), (b - i) / (0.02 * sr) });
                double v = 0.0;
                for (int h = 1; h <= 8; ++h) v += std::sin(h * phase) / h;
                hum.setSample(0, i, float(0.15 * env * v));
            }
        }
        const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("opendaw_hum_test.wav");
        wav.deleteFile();
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream(wav.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), sr, 1, 16, {}, 0));
            if (writer) {
                stream.release();
                writer->writeFromAudioSampleBuffer(hum, 0, hum.getNumSamples());
            }
        }
        auto* vocal = editMgr.addAudioTrack();
        vocal->setName("Hum");
        editMgr.addAudioClipToTrack(*vocal, wav, 4.0);   // bar 2
        EXPECT(vocal->getClips().size() == 1);
        const int tracksBefore = te::getAudioTracks(*editMgr.edit()).size();

        um.clearUndoHistory();
        // An edit just before: undoing the conversion must not take it too.
        um.beginNewTransaction("Rename");
        vocal->setName("Hum");
        vocal->state.setProperty("testMarker", 1, &um);
        const auto r = tools.execute(AiToolCall{ "m1", "transcribe_melody", QJsonObject{ { "track", "Hum" }, { "clip_index", 0 } } });
        if (r.isError) std::printf("    (transcribe_melody error: %s)\n", r.content.toUtf8().constData());
        EXPECT(!r.isError);
        const auto out = QJsonDocument::fromJson(r.content.toUtf8()).object();
        const auto notes = out["notes"].toArray();
        EXPECT(notes.size() == 4);
        bool pitchesOk = notes.size() == 4, timingOk = notes.size() == 4;
        for (int k = 0; k < notes.size() && k < 4; ++k) {
            const auto n = notes[k].toObject();
            pitchesOk &= n["note"].toInt() == pitches[k];
            // Beat k (+0.5 for the 0.25 s lead-in), from the clip start.
            timingOk &= std::abs(n["start_beat"].toDouble() - (0.5 + k)) < 0.08
                        && std::abs(n["length_beats"].toDouble() - 0.84) < 0.1;
        }
        EXPECT(pitchesOk);
        EXPECT(timingOk);
        EXPECT(notes.size() > 0 && notes[0].toObject()["name"] == "E4");
        EXPECT(out["midi_track"] == "Hum Melody");
        EXPECT(out["clip_start_beat"].toDouble() == 4.0);
        EXPECT(out["tempo_bpm"].toDouble() == 120.0);
        EXPECT(!out.contains("low_confidence"));
        EXPECT(te::getAudioTracks(*editMgr.edit()).size() == tracksBefore + 1);
        auto* melodyTrack = te::getAudioTracks(*editMgr.edit()).getLast();
        EXPECT(dynamic_cast<ModularSynthPlugin*>(editMgr.getTrackInstrument(melodyTrack)) != nullptr);
        auto* melodyClip = melodyTrack ? dynamic_cast<te::MidiClip*>(melodyTrack->getClips().getFirst()) : nullptr;
        EXPECT(melodyClip && std::abs(melodyClip->getStartBeat().inBeats() - 4.0) < 1.0e-6);
        EXPECT(melodyClip && melodyClip->getSequence().getNumNotes() == 4);
        // In the clip, notes sit from the clip start, as reported.
        EXPECT(melodyClip && melodyClip->getSequence().getNumNotes() > 0
               && std::abs(melodyClip->getSequence().getNote(0)->getStartBeat().inBeats() - 0.5) < 0.08);
        EXPECT(countUndoSteps(um) == 2);   // the marker, then the conversion
        um.undo();
        EXPECT(te::getAudioTracks(*editMgr.edit()).size() == tracksBefore);   // one step undoes it all
        EXPECT(int(vocal->state.getProperty("testMarker", 0)) == 1);          // and only it

        // A clip trimmed past the first note (it ends at 0.67 s): notes are
        // timed from where the clip starts playing.
        auto* hc = vocal->getClips().getFirst();
        hc->setStart(hc->getPosition().getStart() + tracktion::TimeDuration::fromSeconds(0.7), true, false);
        const auto trimmed = QJsonDocument::fromJson(tools.execute(AiToolCall{ "m2", "transcribe_melody",
            QJsonObject{ { "track", "Hum" }, { "clip_index", 0 } } }).content.toUtf8()).object();
        const auto tn = trimmed["notes"].toArray();
        EXPECT(tn.size() == 3 && tn[0].toObject()["note"].toInt() == 67);
        EXPECT(tn.size() == 3 && std::abs(tn[0].toObject()["start_beat"].toDouble() - 0.1) < 0.08);   // G at 0.75 s
        um.undo();

        // Errors change nothing.
        auto* midiOnly = editMgr.addMidiTrack();
        midiOnly->setName("Keys2");
        editMgr.addMidiClipToTrack(*midiOnly, 0.0, 4.0);
        const auto notAudio = tools.execute(AiToolCall{ "m3", "transcribe_melody", QJsonObject{ { "track", "Keys2" }, { "clip_index", 0 } } });
        EXPECT(notAudio.isError && notAudio.content.contains("not an audio clip"));
        juce::AudioBuffer<float> silence(1, int(sr));
        silence.clear();
        const auto quiet = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("opendaw_silence_test.wav");
        quiet.deleteFile();
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream(quiet.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), sr, 1, 16, {}, 0));
            if (writer) {
                stream.release();
                writer->writeFromAudioSampleBuffer(silence, 0, silence.getNumSamples());
            }
        }
        auto* empty = editMgr.addAudioTrack();
        empty->setName("Quiet");
        editMgr.addAudioClipToTrack(*empty, quiet, 0.0);
        const int before = te::getAudioTracks(*editMgr.edit()).size();
        um.clearUndoHistory();
        const auto none = tools.execute(AiToolCall{ "m4", "transcribe_melody", QJsonObject{ { "track", "Quiet" }, { "clip_index", 0 } } });
        EXPECT(none.isError && none.content.contains("No melody found"));
        EXPECT(te::getAudioTracks(*editMgr.edit()).size() == before && !um.canUndo());

        // ── Snapping to a scale (the Hum button always does) ──
        std::printf("snap to scale\n");
        {
            auto make = [](std::vector<std::pair<int, double>> pc) {
                Melody::Result r;
                for (auto [p, c] : pc) { Melody::Note n; n.pitch = p; n.cents = c; r.notes.push_back(n); }
                return r;
            };
            // In C major: F# sung sharp -> G, C# sung flat -> C, Bb dead on -> B (up), E stays.
            auto r = make({ { 66, 20.0 }, { 61, -30.0 }, { 70, 0.0 }, { 64, 5.0 } });
            std::vector<MelodyToMidi::SnapMove> moved;
            QString error;
            EXPECT(MelodyToMidi::snapToScale(r, "C major", &moved, &error) == "C major");
            EXPECT(r.notes[0].pitch == 67 && r.notes[1].pitch == 60 && r.notes[2].pitch == 71 && r.notes[3].pitch == 64);
            EXPECT(moved.size() == 3 && moved[0].index == 0 && moved[0].fromPitch == 66 && moved[2].fromPitch == 70);
            EXPECT(std::abs(r.notes[0].cents - (-80.0)) < 1.0e-9);   // now 80 cents under G
            // A gap of two: the nearer note, whichever way it leaned.
            auto penta = make({ { 65, 40.0 } });
            EXPECT(MelodyToMidi::snapToScale(penta, "C major pentatonic", nullptr, &error).contains("pentatonic"));
            EXPECT(penta.notes[0].pitch == 64);
            // "auto": the detected key.
            auto detected = make({ { 70, 0.0 } });
            detected.keyTonic = 9;
            detected.keyMinor = true;
            EXPECT(MelodyToMidi::snapToScale(detected, "auto", nullptr, &error).contains("A"));
            EXPECT(detected.notes[0].pitch == 71);
            auto bad = make({ { 61, 0.0 } });
            EXPECT(MelodyToMidi::snapToScale(bad, "Q flurb", nullptr, &error).isEmpty() && error.contains("Unknown scale"));
            EXPECT(bad.notes[0].pitch == 61);

            // Through the tool: E G A G is in A minor, so nothing moves; a bad scale changes nothing.
            const int tracksNow = te::getAudioTracks(*editMgr.edit()).size();
            const auto badTool = tools.execute(AiToolCall{ "s1", "transcribe_melody",
                QJsonObject{ { "track", "Hum" }, { "clip_index", 0 }, { "snap_to_scale", "Q flurb" } } });
            EXPECT(badTool.isError && badTool.content.contains("Unknown scale"));
            EXPECT(te::getAudioTracks(*editMgr.edit()).size() == tracksNow);
            const auto snapped = QJsonDocument::fromJson(tools.execute(AiToolCall{ "s2", "transcribe_melody",
                QJsonObject{ { "track", "Hum" }, { "clip_index", 0 }, { "snap_to_scale", "D major" } } }).content.toUtf8()).object();
            // G and A are in D major: nothing moves.
            EXPECT(snapped["snapped_to"].toString().startsWith("D") && snapped["snapped_count"].toInt() == 0);
            const auto flat = QJsonDocument::fromJson(tools.execute(AiToolCall{ "s3", "transcribe_melody",
                QJsonObject{ { "track", "Hum" }, { "clip_index", 0 }, { "snap_to_scale", "Eb major" } } }).content.toUtf8()).object();
            // The clip is trimmed to G A G by now. In Eb major only the A moves: sung dead on, between Ab and Bb, it goes up.
            int moves = 0;
            for (const auto& n : flat["notes"].toArray())
                if (n.toObject().contains("snapped_from")) ++moves;
            EXPECT(flat["snapped_count"].toInt() == 1 && moves == 1);
            EXPECT(flat["notes"].toArray().size() == 3 && flat["notes"].toArray()[1].toObject()["note"].toInt() == 70);
        }

        // ── The Hum button: track set-up, and the take goes to Convert to MIDI ──
        std::printf("hum recorder\n");
        EXPECT(HumRecorder::noteName(440.0) == "A4");
        EXPECT(HumRecorder::noteName(440.0 * std::pow(2.0, 0.12 / 12.0)) == QString::fromUtf8("A4 +12¢"));
        EXPECT(HumRecorder::noteName(261.6256 * std::pow(2.0, -0.3 / 12.0)) == QString::fromUtf8("C4 -30¢"));
        EXPECT(HumRecorder::noteName(0.0).isEmpty());
        {
            auto* edit = editMgr.edit();
            // An audio device, for its inputs (none on a machine without one).
            engine.setDefaultAudioDevice();
            editMgr.enableAllWaveInputDevices();
            edit->getTransport().ensureContextAllocated();
            settle();
            te::InputDeviceInstance* input = nullptr;
            for (auto* in : edit->getAllInputDevices())
                if (in->getInputDevice().getDeviceType() == te::InputDevice::waveDevice) { input = in; break; }
            HumRecorder hum(editMgr);
            if (!input) {
                QString error;
                EXPECT(hum.prepareTrack(&error) == nullptr && error.contains("no audio input"));
                std::printf("    (no audio input here: only the no-input case was checked)\n");
            } else {
                // The user's own track has the input and is armed.
                auto* mine = editMgr.addAudioTrack();
                mine->setName("Vocals");
                (void) input->setTarget(mine->itemID, false, &um, 0);
                input->setRecordingEnabled(mine->itemID, true);
                const int before = te::getAudioTracks(*edit).size();
                QString error;
                auto* humTrack = hum.prepareTrack(&error);
                EXPECT(humTrack && humTrack->getName() == "Hum");
                // "Hum" exists already (the melody test's), so it is reused.
                EXPECT(humTrack == vocal && te::getAudioTracks(*edit).size() == before);
                EXPECT(input->getTargets().contains(mine->itemID) && input->getTargets().contains(vocal->itemID));
                EXPECT(input->isRecordingEnabled(vocal->itemID) && !input->isRecordingEnabled(mine->itemID));

                te::WaveAudioClip* converted = nullptr;
                QObject::connect(&editMgr, &EditManager::humTakeRecorded, &hum,
                                 [&](te::WaveAudioClip* c) { converted = c; });
                // A clip from another track is not the take.
                editMgr.addAudioClipToTrack(*mine, wav, 0.0);
                hum.takeRecorded({ mine->getClips().getLast() });
                settle();
                EXPECT(converted == nullptr && input->isRecordingEnabled(vocal->itemID));
                auto* take = dynamic_cast<te::WaveAudioClip*>(vocal->getClips().getFirst());
                hum.takeRecorded({ mine->getClips().getLast(), take });
                settle();
                EXPECT(converted == take);
                // Arming is back as it was.
                EXPECT(input->isRecordingEnabled(mine->itemID) && !input->isRecordingEnabled(vocal->itemID));
                EXPECT(hum.state() == HumRecorder::State::Idle);

                // No Hum track yet: one is added.
                vocal->setName("Hum take");
                auto* fresh = hum.prepareTrack(&error);
                EXPECT(fresh && fresh != vocal && fresh->getName() == "Hum"
                       && te::getAudioTracks(*edit).size() == before + 1);
                EXPECT(fresh && input->isRecordingEnabled(fresh->itemID));

                // A real take: count-in, recording, stop -> Convert to MIDI.
                converted = nullptr;
                QStringList readouts;
                QObject::connect(&hum, &HumRecorder::readoutChanged, &hum,
                                 [&](const QString& t) { readouts << t; });
                edit->setCountInMode(te::Edit::CountIn::none);
                edit->getTransport().setPosition(tracktion::TimePosition::fromSeconds(0.0));
                EXPECT(hum.start(&error));
                EXPECT(hum.state() == HumRecorder::State::CountIn);
                EXPECT(edit->getCountInMode() == te::Edit::CountIn::oneBar);
                QElapsedTimer clock;
                clock.start();
                while (clock.elapsed() < 6000 && hum.state() != HumRecorder::State::Recording) settle(20);
                EXPECT(hum.state() == HumRecorder::State::Recording);
                EXPECT(clock.elapsed() > 1500);   // one bar at 120 BPM is 2 s
                settle(600);
                hum.stop();
                settle(300);
                EXPECT(hum.state() == HumRecorder::State::Idle && !edit->getTransport().isRecording());
                EXPECT(converted != nullptr && converted->getTrack() == fresh);
                EXPECT(edit->getCountInMode() == te::Edit::CountIn::none);
                EXPECT(input->isRecordingEnabled(mine->itemID) && !input->isRecordingEnabled(fresh->itemID));
                EXPECT(readouts.size() >= 3 && readouts.first() == "Count-in 4" && readouts.last().isEmpty());
                if (converted) converted->getSourceFileReference().getFile().deleteFile();
            }
        }

        wav.deleteFile();
        quiet.deleteFile();
    }

    // ── Undoing a new track with the timeline and mixer open (it crashed) ──
    std::printf("undo a new track\n");
    {
        TimelineView timeline(&editMgr);
        MixerView mixer(&editMgr);
        timeline.resize(1200, 700);
        timeline.show();
        mixer.show();
        settle();
        const int before = te::getAudioTracks(*editMgr.edit()).size();
        um.beginNewTransaction("Add track");
        auto* added = editMgr.addAudioTrack();
        added->setName("Undo me");
        editMgr.addMidiClipToTrack(*added, 0.0, 4.0);
        settle();
        editMgr.undo();
        settle();
        EXPECT(te::getAudioTracks(*editMgr.edit()).size() == before);
        editMgr.redo();
        settle();
        editMgr.undo();
        settle();
        EXPECT(te::getAudioTracks(*editMgr.edit()).size() == before);
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    view.setTrack(nullptr);
    return failures ? 1 : 0;
}
