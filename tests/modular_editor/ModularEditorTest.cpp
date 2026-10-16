// Offscreen checks of the Modular patch editor against a real Edit and
// Modular Synth: the scene matches the patch, mouse gestures go through
// applyOps/setPatch as exactly one undo step, and outside changes (AI,
// undo) rebuild the view. Run with QT_QPA_PLATFORM=offscreen.

#include <cmath>
#include "app/JuceQtBridge.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/modular/ModularSynthPlugin.h"
#include "ui/controls/RotaryKnob.h"
#include "modular/FaustStarters.h"
#include "ui/modular/FaustCodeDialog.h"
#include "ui/modular/ModularEditorView.h"
#include "ui/modular/ModulePanelItem.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsView>
#include <QMouseEvent>
#include <QPushButton>
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
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
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

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
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

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    view.setTrack(nullptr);
    return failures ? 1 : 0;
}
