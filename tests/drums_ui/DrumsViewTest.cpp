// Offscreen checks of the Drums tab against a real Edit and Drum Machine:
// the grid matches the kit, gestures and drops go through setKit as exactly
// one undo step, outside changes (AI, undo) rebuild the view, and auditions
// reach the engine. Run with QT_QPA_PLATFORM=offscreen.

#include "app/JuceQtBridge.h"
#include "drums/SampleLibrary.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/drums/DrumMachinePlugin.h"
#include "ui/controls/RotaryKnob.h"
#include "ui/drums/DrumPadEditor.h"
#include "ui/drums/DrumPadWidget.h"
#include "ui/drums/DrumsView.h"
#include "ui/drums/DrumsViewLink.h"
#include "ui/drums/SampleBrowser.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTreeView>
#include <cmath>
#include <cstdio>

using namespace OpenDaw;
namespace D = OpenDaw::Drums;

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

// Drags a knob straight up by `pixels`, one event per 4 px, as a user would.
void dragKnob(RotaryKnob* knob, int pixels, bool release = true)
{
    const QPoint c = knob->rect().center();
    send(knob, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    for (int dy = 4; dy <= pixels; dy += 4)
        send(knob, QEvent::MouseMove, c - QPoint(0, dy), Qt::NoButton, Qt::LeftButton);
    if (release)
        send(knob, QEvent::MouseButtonRelease, c - QPoint(0, pixels), Qt::LeftButton, Qt::NoButton);
}

void drop(QWidget* target, const QList<QUrl>& urls)
{
    QMimeData mime;
    mime.setUrls(urls);
    const QPointF pos = QRectF(target->rect()).center();
    QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDropEvent ev(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
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

bool writeWav(const juce::File& file, double seconds)
{
    constexpr double rate = 44100.0;
    const int n = int(seconds * rate);
    juce::AudioBuffer<float> buffer(1, n);
    for (int i = 0; i < n; ++i)
        buffer.setSample(0, i, 0.5f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate))
                                  * float(1.0 - double(i) / n));
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out(file.createOutputStream().release());
    if (!out) return false;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1)
                                                         .withBitsPerSample(16);
    auto writer = wav.createWriterFor(out, options);
    return writer && writer->writeFromAudioSampleBuffer(buffer, 0, n);
}

// Renders one block through the plugin as the audio thread would.
juce::AudioBuffer<float> render(DrumMachinePlugin& plugin, int numSamples, te::MidiMessageArray* midi = nullptr)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    buffer.clear();
    te::PluginRenderContext rc(&buffer, juce::AudioChannelSet::stereo(), 0, numSamples, midi, 0.0,
                               {}, true, false, false, false);
    plugin.applyToBuffer(rc);
    return buffer;
}

float peak(const juce::AudioBuffer<float>& b)
{
    return std::max(b.getMagnitude(0, 0, b.getNumSamples()), b.getMagnitude(1, 0, b.getNumSamples()));
}

float maxDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float d = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            d = std::max(d, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
    return d;
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Own settings, so the user's samples folder is never touched.
    QCoreApplication::setOrganizationName("OpenDawTests");
    QCoreApplication::setApplicationName("DrumsViewTest");
    juce::ScopedJuceInitialiser_GUI juceInit;

    JuceQtBridge bridge;
    bridge.start();
    AudioEngine engine;
    EditManager editMgr(engine);
    editMgr.stopAutosave();

    auto* track = editMgr.addMidiTrack();
    auto* dm = track ? dynamic_cast<DrumMachinePlugin*>(editMgr.setTrackDrumMachine(*track)) : nullptr;
    if (!dm) {
        std::printf("FAIL: could not create a Drum Machine\n");
        return 1;
    }
    auto& um = editMgr.edit()->getUndoManager();
    QTemporaryDir tmp;
    const QString samplesDir = QDir(tmp.path()).filePath("Samples");
    D::setSamplesFolder(samplesDir);

    DrumsView view(&editMgr);
    view.resize(1200, 600);
    view.show();
    view.setTrack(track);
    settle();
    auto* editor = view.padEditor();

    const auto defaults = D::Kit::defaultKit();
    std::printf("default kit in the view\n");
    EXPECT(view.drumMachine() == dm);
    EXPECT(view.padCount() == int(defaults.pads.size()));
    EXPECT(view.padCount() == 20);
    EXPECT(view.padWidget(D::Gm::Kick) && view.padWidget(D::Gm::Kick)->sourceText() == "kick");
    EXPECT(view.warningText().isEmpty());

    // ── Clicking a pad selects and auditions it ──
    std::printf("click a pad\n");
    render(*dm, 64);   // nothing queued yet
    send(view.padWidget(D::Gm::Snare), QEvent::MouseButtonPress, { 10, 10 }, Qt::LeftButton, Qt::LeftButton);
    send(view.padWidget(D::Gm::Snare), QEvent::MouseButtonRelease, { 10, 10 }, Qt::LeftButton, Qt::NoButton);
    settle();
    EXPECT(view.selectedNote() == D::Gm::Snare);
    EXPECT(editor->note() == D::Gm::Snare);
    EXPECT(view.padWidget(D::Gm::Snare)->isSelected());
    EXPECT(!view.padWidget(D::Gm::Kick)->isSelected());
    EXPECT(dm->pendingAuditions() == 1);
    render(*dm, 64);
    EXPECT(dm->pendingAuditions() == 0);

    // ── A mix knob drag is one undo step and touches the kit only on release ──
    std::printf("level knob gesture\n");
    um.clearUndoHistory();
    auto* level = editor->mixKnob("level");
    EXPECT(level != nullptr);
    const float levelBefore = dm->kit().padForNote(D::Gm::Snare)->level;
    const int rebuildsBefore = view.rebuildCount();
    const auto& levelDef = DrumPadEditor::levelDef();
    dragKnob(level, 40, false);
    settle();
    EXPECT(!um.canUndo());   // nothing stored while dragging
    EXPECT(std::abs(dm->kit().padForNote(D::Gm::Snare)->level - levelBefore) <= 0.0f);   // untouched
    EXPECT(view.rebuildCount() == rebuildsBefore);
    send(level, QEvent::MouseButtonRelease, level->rect().center() - QPoint(0, 40), Qt::LeftButton,
         Qt::NoButton);
    settle();
    // RotaryKnob moves 1/150 of its 0..1 range per pixel, so 40 px is
    // 40/150 of level's 0..1.5 range. One pixel is 0.01 of level; the kit
    // stores a float through JSON (error ~1e-7), so 1e-4 catches any
    // off-by-a-pixel or wrong-mapping bug while ignoring rounding.
    const float levelExpected = levelBefore + float((levelDef.max - levelDef.min) * 40.0 / 150.0);
    const float levelAfter = dm->kit().padForNote(D::Gm::Snare)->level;
    std::printf("  level %.4f -> %.4f (expected %.4f)\n", double(levelBefore), double(levelAfter),
                double(levelExpected));
    EXPECT(std::abs(levelAfter - levelExpected) < 1e-4f);
    EXPECT(countUndoSteps(um) == 1);
    EXPECT(um.getUndoDescription() == "Change Pad");
    EXPECT(view.rebuildCount() == rebuildsBefore + 1);   // one rebuild for the gesture

    // ── A voice parameter knob uses the modular log mapping ──
    std::printf("voice parameter knob\n");
    um.clearUndoHistory();
    auto* tune = editor->paramKnob("tune");   // the snare voice's body pitch (Hz)
    EXPECT(tune != nullptr);
    if (tune) {
        const auto* snareDef = Modular::findModule("snare");
        const auto& tuneDef = snareDef->params[size_t(snareDef->paramIndex("tune"))];
        const auto* pad = dm->kit().padForNote(D::Gm::Snare);
        const float hzBefore = pad->params.value("tune", tuneDef.defaultValue);
        dragKnob(tune, 20);
        settle();
        const float hzAfter = dm->kit().padForNote(D::Gm::Snare)->params.value("tune", -1.0f);
        // Hz knobs are logarithmic: 20 px of the 150 px travel multiplies
        // the frequency by (max/min)^(20/150), computed here independently.
        const double hzExpected = hzBefore * std::pow(double(tuneDef.max) / tuneDef.min, 20.0 / 150.0);
        // Pitch error that matters is a few cents; 1 cent is a ratio of
        // 2^(1/1200), and float/JSON rounding is ~1e-7 relative.
        const double cents = 1200.0 * std::log2(double(hzAfter) / hzExpected);
        std::printf("  snare tune %.2f Hz -> %.2f Hz (expected %.2f, %.4f cents off)\n", double(hzBefore),
                    double(hzAfter), hzExpected, cents);
        EXPECT(std::abs(cents) < 0.5);
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Change Pad");
    }

    // ── Changing the voice is one step and resets its parameters ──
    std::printf("change voice\n");
    um.clearUndoHistory();
    {
        auto* box = editor->voiceBox();
        const int clap = box->findData("clap");
        EXPECT(clap >= 0);
        box->setCurrentIndex(clap);
        emit box->activated(clap);
        settle();
        const auto* pad = dm->kit().padForNote(D::Gm::Snare);
        EXPECT(pad->module == "clap" && pad->params.isEmpty());
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Change Voice");
        EXPECT(view.padWidget(D::Gm::Snare)->sourceText() == "clap");
        EXPECT(editor->paramKnob("tune") == nullptr || Modular::findModule("clap")->paramIndex("tune") >= 0);
        um.undo();
        settle();
        EXPECT(dm->kit().padForNote(D::Gm::Snare)->module == "snare");
        EXPECT(view.padWidget(D::Gm::Snare)->sourceText() == "snare");
    }

    // ── Dropping a WAV on a pad makes it a sample pad ──
    std::printf("drop a sample on a pad\n");
    um.clearUndoHistory();
    const juce::File wav(juce::String(QDir(tmp.path()).filePath("hit.wav").toStdString()));
    EXPECT(writeWav(wav, 0.25));
    const QString wavPath = QString::fromStdString(wav.getFullPathName().toStdString());
    drop(view.padWidget(D::Gm::Clap), { QUrl::fromLocalFile(wavPath) });
    settle();
    {
        const auto* pad = dm->kit().padForNote(D::Gm::Clap);
        EXPECT(pad && pad->source == D::PadSource::Sample);
        EXPECT(pad && pad->sample == wavPath);
        EXPECT(pad && pad->name == "Clap");   // the pad keeps its name
        EXPECT(dm->warnings().isEmpty());
        EXPECT(view.warningText().isEmpty());
        EXPECT(view.padWidget(D::Gm::Clap) && view.padWidget(D::Gm::Clap)->sourceText() == "hit.wav");
        EXPECT(view.padWidget(D::Gm::Clap) && view.padWidget(D::Gm::Clap)->warning().isEmpty());
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Load Sample");
    }

    // ── Other files are refused with a message ──
    std::printf("drop a non-audio file\n");
    um.clearUndoHistory();
    drop(view.padWidget(D::Gm::Kick), { QUrl::fromLocalFile(QDir(tmp.path()).filePath("notes.txt")) });
    settle();
    EXPECT(!view.lastError().isEmpty());
    EXPECT(!um.canUndo());
    EXPECT(dm->kit().padForNote(D::Gm::Kick)->source == D::PadSource::Synth);
    drop(view.padWidget(D::Gm::Kick), { QUrl::fromLocalFile(wavPath), QUrl::fromLocalFile(wavPath) });
    EXPECT(!um.canUndo());   // two files at once

    // ── A drop on empty space adds a pad on the next free GM note ──
    std::printf("drop on empty space\n");
    um.clearUndoHistory();
    QWidget* area = nullptr;
    for (auto* w : view.findChildren<QWidget*>())
        if (w->accessibleName() == "Drum Pads") area = w;
    EXPECT(area != nullptr);
    if (area) drop(area, { QUrl::fromLocalFile(wavPath) });
    settle();
    EXPECT(view.padCount() == 21);
    {
        const auto* pad = dm->kit().padForNote(35);   // the default kit starts at 36
        EXPECT(pad && pad->source == D::PadSource::Sample && pad->sample == wavPath && pad->name == "hit");
        EXPECT(view.selectedNote() == 35);
        EXPECT(countUndoSteps(um) == 1);
    }

    // ── Missing samples show in the banner and on the pad ──
    std::printf("missing sample warning\n");
    {
        auto kit = dm->kit();
        kit.padForNote(35)->sample = QDir(tmp.path()).filePath("gone.wav");
        dm->setKit(kit, "AI: Edit Drum Kit");
        settle();
        EXPECT(!dm->warnings().isEmpty());
        EXPECT(view.warningText().contains("gone.wav"));
        EXPECT(view.padWidget(35) && !view.padWidget(35)->warning().isEmpty());
        EXPECT(view.padWidget(36) && view.padWidget(36)->warning().isEmpty());
        um.undo();
        settle();
        EXPECT(view.warningText().isEmpty());
        EXPECT(view.padWidget(35) && view.padWidget(35)->warning().isEmpty());
    }

    // ── An outside edit (as the AI makes) and undo rebuild the view ──
    std::printf("live sync\n");
    um.clearUndoHistory();
    {
        auto kit = dm->kit();
        kit.pads.removeIf([](const D::Pad& p) { return p.note == D::Gm::Cowbell; });
        kit.padForNote(D::Gm::Kick)->name = "Big Kick";
        const int rebuilds = view.rebuildCount();
        dm->setKit(kit, "AI: Edit Drum Kit");
        settle();
        EXPECT(view.rebuildCount() > rebuilds);
        EXPECT(view.padCount() == 20);
        EXPECT(view.padWidget(D::Gm::Cowbell) == nullptr);
        EXPECT(view.padWidget(D::Gm::Kick)->accessibleName() == "Pad 36 Big Kick");
        const int afterAi = view.rebuildCount();
        um.undo();
        settle();
        EXPECT(view.rebuildCount() > afterAi);
        EXPECT(view.padCount() == 21);
        EXPECT(view.padWidget(D::Gm::Cowbell) != nullptr);
        EXPECT(view.padWidget(D::Gm::Kick)->accessibleName() == "Pad 36 Kick");
    }

    // ── Add Pad, and the pad menu's actions, are one step each ──
    std::printf("add / rename / duplicate / reset / remove\n");
    {
        um.clearUndoHistory();
        QPushButton* add = nullptr;
        for (auto* b : view.findChildren<QPushButton*>())
            if (b->accessibleName() == "Add Pad") add = b;
        EXPECT(add != nullptr);
        if (add) add->click();
        settle();
        EXPECT(view.padCount() == 22);
        EXPECT(dm->kit().padForNote(52) != nullptr);   // 52 (China) is the first free GM note
        EXPECT(countUndoSteps(um) == 1 && um.getUndoDescription() == "Add Pad");

        um.clearUndoHistory();
        EXPECT(view.renamePad(D::Gm::Kick, "  Thump "));
        settle();
        EXPECT(dm->kit().padForNote(D::Gm::Kick)->name == "Thump");
        EXPECT(view.duplicatePad(D::Gm::Kick, 60));
        settle();
        EXPECT(dm->kit().padForNote(60) && dm->kit().padForNote(60)->module == "kick");
        EXPECT(!view.duplicatePad(D::Gm::Kick, D::Gm::Snare));   // taken
        EXPECT(view.resetPad(D::Gm::Kick));
        settle();
        EXPECT(dm->kit().padForNote(D::Gm::Kick)->name == "Kick");
        EXPECT(!view.resetPad(60));   // not in the default kit
        EXPECT(view.removePad(60));
        settle();
        EXPECT(dm->kit().padForNote(60) == nullptr);
        EXPECT(countUndoSteps(um) == 4);
    }

    // ── Auditions reach the engine exactly like a MIDI note ──
    std::printf("audition\n");
    {
        // Two fresh default kits: one auditioned, one played by MIDI.
        auto* trackA = editMgr.addMidiTrack();
        auto* trackB = editMgr.addMidiTrack();
        auto* a = dynamic_cast<DrumMachinePlugin*>(editMgr.setTrackDrumMachine(*trackA));
        auto* b = dynamic_cast<DrumMachinePlugin*>(editMgr.setTrackDrumMachine(*trackB));
        EXPECT(a && b);
        if (a && b) {
            constexpr int n = 2048;
            EXPECT(peak(render(*a, n)) <= 0.0f);   // idle kit: digital silence
            // A MIDI velocity is 7-bit, so audition with the same value.
            const float velocity = 102.0f / 127.0f;
            a->auditionNote(D::Gm::Kick, velocity);
            EXPECT(a->pendingAuditions() == 1);
            const auto auditioned = render(*a, n);
            EXPECT(a->pendingAuditions() == 0);

            te::MidiMessageArray midi;
            midi.addMidiMessage(juce::MidiMessage::noteOn(10, D::Gm::Kick, juce::uint8(102)), 0.0, te::MPESourceID{});
            const auto played = render(*b, n, &midi);

            // Same code path from the same sample: identical apart from float
            // noise. The kick peaks around -6 dBFS; 0.05 (-26 dB) is far above
            // the silence a dropped audition would leave.
            const float diff = maxDifference(auditioned, played);
            std::printf("  audition peak %.3f, MIDI peak %.3f, max difference %g\n",
                        double(peak(auditioned)), double(peak(played)), double(diff));
            EXPECT(peak(auditioned) > 0.05f);
            EXPECT(diff <= 1e-6f);

            // A full queue drops hits instead of blocking or allocating.
            for (int i = 0; i < 100; ++i) a->auditionNote(D::Gm::Snare, 0.5f);
            EXPECT(a->pendingAuditions() < 100 && a->pendingAuditions() > 0);
            render(*a, 64);
            EXPECT(a->pendingAuditions() == 0);

            // Hits queued while the audio isn't running don't pile up for later.
            auto* trackC = editMgr.addMidiTrack();
            auto* c = dynamic_cast<DrumMachinePlugin*>(editMgr.setTrackDrumMachine(*trackC));
            EXPECT(c != nullptr);
            if (c) {
                c->auditionNote(D::Gm::Kick, velocity);
                settle(DrumMachinePlugin::kAuditionMaxAgeMs + 100);
                EXPECT(peak(render(*c, n)) <= 0.0f);
                EXPECT(c->pendingAuditions() == 0);
                c->auditionNote(D::Gm::Kick, velocity);   // control: a fresh one plays
                EXPECT(peak(render(*c, n)) > 0.05f);
            }
        }
    }

    // ── Samples in the samples folder are stored relative to it ──
    std::printf("samples folder\n");
    um.clearUndoHistory();
    {
        QDir().mkpath(samplesDir + "/808");
        const QString kickPath = samplesDir + "/808/kick.wav";
        EXPECT(writeWav(juce::File(juce::String(kickPath.toStdString())), 0.25));
        view.sampleBrowser()->refresh();
        EXPECT(view.sampleBrowser()->folder() == samplesDir);
        EXPECT(view.sampleBrowser()->tree()->isVisibleTo(&view));

        drop(view.padWidget(D::Gm::Kick), { QUrl::fromLocalFile(kickPath) });
        settle();
        const auto* pad = dm->kit().padForNote(D::Gm::Kick);
        EXPECT(pad && pad->source == D::PadSource::Sample && pad->sample == "808/kick.wav");
        EXPECT(dm->warnings().isEmpty());
        EXPECT(view.padWidget(D::Gm::Kick) && view.padWidget(D::Gm::Kick)->sourceText() == "kick.wav");

        // The folder moves (or the project opens elsewhere): the kit follows it.
        const QString moved = QDir(tmp.path()).filePath("Moved Samples");
        EXPECT(QDir().rename(samplesDir, moved));
        D::setSamplesFolder(moved);
        dm->reloadSamples();
        DrumsViewLink::instance().notifySamplesFolderChanged();
        settle();
        EXPECT(dm->warnings().isEmpty());
        EXPECT(dm->kit().padForNote(D::Gm::Kick)->sample == "808/kick.wav");
        EXPECT(view.sampleBrowser()->folder() == moved);

        // A folder without the file: the pad is reported missing, in the banner too.
        D::setSamplesFolder(QDir(tmp.path()).filePath("Empty"));
        dm->reloadSamples();
        DrumsViewLink::instance().notifySamplesFolderChanged();
        settle();
        EXPECT(dm->warnings().join(" ").contains("808/kick.wav"));
        EXPECT(view.warningText().contains("808/kick.wav"));
        EXPECT(view.padWidget(D::Gm::Kick) && !view.padWidget(D::Gm::Kick)->warning().isEmpty());

        D::setSamplesFolder(moved);
        dm->reloadSamples();
        DrumsViewLink::instance().notifySamplesFolderChanged();
        settle();
        EXPECT(view.warningText().isEmpty());

        // Double-clicking a sample in the browser loads it on the selected pad.
        um.clearUndoHistory();
        view.selectPad(D::Gm::Snare, false);
        emit view.sampleBrowser()->sampleActivated(moved + "/808/kick.wav");
        settle();
        pad = dm->kit().padForNote(D::Gm::Snare);
        EXPECT(pad && pad->source == D::PadSource::Sample && pad->sample == "808/kick.wav");
        EXPECT(countUndoSteps(um) == 1);
        // Samples outside the folder keep their absolute path.
        EXPECT(dm->kit().padForNote(D::Gm::Clap)->sample == wavPath);
    }

    // ── Kit presets: save, load a built-in, load it back ──
    std::printf("kit presets\n");
    {
        D::setSamplesFolder(QDir(tmp.path()).filePath("Moved Samples"));
        const auto mine = dm->kit().toJson();
        EXPECT(view.saveKitPreset("My Kit", false));
        EXPECT(QFileInfo::exists(QDir(tmp.path()).filePath("Moved Samples/Kits/My Kit.drumkit")));
        EXPECT(!view.saveKitPreset("my kit", false));   // already there
        EXPECT(view.lastError().contains("already exists"));
        EXPECT(!view.saveKitPreset("Synth 808", true));   // a built-in's name

        um.clearUndoHistory();
        EXPECT(view.loadKitPreset("Synth 808"));
        settle();
        EXPECT(countUndoSteps(um) == 1);
        EXPECT(um.getUndoDescription() == "Load Kit: Synth 808");
        EXPECT(dm->kit().padForNote(D::Gm::Snare)->source == D::PadSource::Synth);
        EXPECT(view.padWidget(D::Gm::Snare) && view.padWidget(D::Gm::Snare)->sourceText() == "snare");
        EXPECT(view.loadKitPreset("My Kit"));
        settle();
        EXPECT(dm->kit().toJson() == mine);
        EXPECT(dm->warnings().isEmpty());
        EXPECT(!view.loadKitPreset("Nope"));
        EXPECT(view.lastError().contains("Synth 909"));
    }

    // ── A track without a Drum Machine shows the Load button ──
    std::printf("empty state\n");
    {
        auto* plain = editMgr.addMidiTrack();
        editMgr.removeTrackInstrument(*plain);
        view.setTrack(plain);
        settle();
        EXPECT(view.drumMachine() == nullptr);
        QPushButton* load = nullptr;
        for (auto* b : view.findChildren<QPushButton*>())
            if (b->accessibleName() == "Load Drum Machine" && b->isVisibleTo(&view)) load = b;
        EXPECT(load != nullptr);
        if (load) load->click();
        settle();
        EXPECT(view.drumMachine() != nullptr);
        EXPECT(view.padCount() == int(defaults.pads.size()));
        view.setTrack(track);
        settle();
        EXPECT(view.drumMachine() == dm);
    }

    std::printf(failures == 0 ? "\nAll Drums tab checks passed.\n" : "\n%d check(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
