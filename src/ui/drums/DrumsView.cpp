#include "DrumsView.h"
#include "DrumPadWidget.h"
#include "GmDrumNames.h"
#include "engine/drums/DrumMachinePlugin.h"
#include "utils/ThemeManager.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace OpenDaw {

namespace {

// DrumMachinePlugin keeps its kit JSON under this property of its state.
const juce::Identifier kKitProperty("drumKit");

constexpr int kPadColumns = 4;
constexpr int kErrorFlashMs = 4000;
constexpr int kGestureRetryMs = 100;
constexpr float kAuditionVelocity = 0.8f;

juce::String toJuce(const QString& s)
{
    return juce::String::fromUTF8(s.toUtf8().constData());
}

// The empty space around the pads: a drop there adds a pad.
class PadGridArea : public QWidget {
public:
    std::function<void(const QList<QUrl>&)> onDrop;

    explicit PadGridArea(QWidget* parent) : QWidget(parent)
    {
        setAcceptDrops(true);
        setAccessibleName("Drum Pads");
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override
    {
        if (!event->mimeData()->hasUrls()) return;
        event->acceptProposedAction();
        if (onDrop) onDrop(event->mimeData()->urls());
    }
};

} // namespace

DrumsView::DrumsView(EditManager* editMgr, QWidget* parent)
    : QWidget(parent), editMgr_(editMgr)
{
    setAccessibleName("Drums Editor");
    auto& theme = ThemeManager::instance().current();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    layout->addWidget(stack_);

    // ── Empty state ──
    auto* emptyPage = new QWidget(stack_);
    auto* emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch();
    emptyLabel_ = new QLabel(emptyPage);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme.textDim.name()));
    emptyLayout->addWidget(emptyLabel_);
    loadButton_ = new QPushButton("Load Drum Machine", emptyPage);
    loadButton_->setAccessibleName("Load Drum Machine");
    loadButton_->setFixedWidth(180);
    emptyLayout->addWidget(loadButton_, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    connect(loadButton_, &QPushButton::clicked, this, [this]() {
        if (!track_ || !trackStillExists()) return;
        editMgr_->setTrackDrumMachine(*track_);
        refreshTarget();
    });
    stack_->addWidget(emptyPage);

    // ── Editor ──
    auto* editorPage = new QWidget(stack_);
    auto* editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(8, 4, 8, 4);
    titleLabel_ = new QLabel(editorPage);
    titleLabel_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(theme.text.name()));
    header->addWidget(titleLabel_);
    auto* hint = new QLabel("Click a pad to play it; drop an audio file on a pad to use it", editorPage);
    hint->setStyleSheet(QString("color: %1; font-size: 10px;").arg(theme.textDim.name()));
    header->addWidget(hint);
    header->addStretch();
    auto* addButton = new QPushButton("Add Pad", editorPage);
    addButton->setAccessibleName("Add Pad");
    addButton->setToolTip("Add a synth pad on the next free General MIDI drum note");
    connect(addButton, &QPushButton::clicked, this, [this]() { addPad(); });
    header->addWidget(addButton);
    editorLayout->addLayout(header);

    warningBar_ = new QLabel(editorPage);
    warningBar_->setAccessibleName("Kit Warnings");
    warningBar_->setWordWrap(true);
    warningBar_->setStyleSheet("background: #4a3f12; color: #ffd866; font-size: 11px; padding: 3px 8px;");
    warningBar_->hide();
    editorLayout->addWidget(warningBar_);

    auto* splitter = new QSplitter(Qt::Horizontal, editorPage);
    auto* gridScroll = new QScrollArea(splitter);
    gridScroll->setWidgetResizable(true);
    gridScroll->setFrameStyle(0);
    auto* area = new PadGridArea(gridScroll);
    area->onDrop = [this](const QList<QUrl>& urls) { dropFiles(-1, urls); };
    gridArea_ = area;
    auto* areaLayout = new QVBoxLayout(gridArea_);
    areaLayout->setContentsMargins(8, 6, 8, 6);
    gridLayout_ = new QGridLayout();
    gridLayout_->setSpacing(6);
    areaLayout->addLayout(gridLayout_);
    areaLayout->addStretch();
    gridScroll->setWidget(gridArea_);
    splitter->addWidget(gridScroll);

    auto* editorScroll = new QScrollArea(splitter);
    editorScroll->setWidgetResizable(true);
    editorScroll->setFrameStyle(0);
    editor_ = new DrumPadEditor(editorScroll);
    editor_->setEditHandler([this](int note, const DrumPadEditor::Change& change, const QString& undoName) {
        editPad(note, change, undoName);
    });
    connect(editor_, &DrumPadEditor::browseRequested, this, &DrumsView::browseSample);
    editorScroll->setWidget(editor_);
    splitter->addWidget(editorScroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    editorLayout->addWidget(splitter, 1);

    // Floats over the pads so a refused edit doesn't shift the layout.
    errorLabel_ = new QLabel(gridScroll);
    errorLabel_->setAccessibleName("Drums Error");
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet("background: rgba(160, 40, 40, 230); color: white; "
                               "padding: 6px 10px; border-radius: 4px; font-size: 11px;");
    errorLabel_->hide();
    errorTimer_.setSingleShot(true);
    errorTimer_.setInterval(kErrorFlashMs);
    connect(&errorTimer_, &QTimer::timeout, errorLabel_, &QLabel::hide);

    stack_->addWidget(editorPage);

    rebuildTimer_.setSingleShot(true);
    rebuildTimer_.setInterval(0);
    connect(&rebuildTimer_, &QTimer::timeout, this, &DrumsView::rebuild);

    connect(editMgr_, &EditManager::aboutToChangeEdit, this, [this]() { setTrack(nullptr); });
    connect(editMgr_, &EditManager::tracksChanged, this, &DrumsView::refreshTarget);
    connect(editMgr_, &EditManager::editChanged, this, &DrumsView::refreshTarget);

    updatePage();
}

DrumsView::~DrumsView()
{
    rebuildTimer_.stop();
    attach(nullptr);
    disconnect(editMgr_, nullptr, this, nullptr);
}

// ── Target track / plugin ───────────────────────────────────────────────────

void DrumsView::setTrack(te::AudioTrack* track)
{
    track_ = track;
    refreshTarget();
}

DrumMachinePlugin* DrumsView::drumMachine() const
{
    return dynamic_cast<DrumMachinePlugin*>(plugin_.get());
}

bool DrumsView::trackStillExists() const
{
    if (!track_ || !editMgr_->edit()) return false;
    return te::getAudioTracks(*editMgr_->edit()).contains(track_);
}

void DrumsView::refreshTarget()
{
    if (track_ && !trackStillExists())
        track_ = nullptr;

    auto* found = track_ ? dynamic_cast<DrumMachinePlugin*>(editMgr_->getTrackInstrument(track_))
                         : nullptr;
    if (found != drumMachine()) {
        attach(found);
        rebuild();
    }
    updatePage();
}

void DrumsView::attach(DrumMachinePlugin* plugin)
{
    if (pluginState_.isValid())
        pluginState_.removeListener(this);
    plugin_ = plugin;
    selectedNote_ = -1;
    pluginState_ = plugin ? plugin->state : juce::ValueTree();
    if (pluginState_.isValid())
        pluginState_.addListener(this);
}

void DrumsView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& id)
{
    // Deferred: the plugin parses the new JSON in its own listener, which
    // may run after this one.
    if (tree == pluginState_ && id == kKitProperty)
        scheduleRebuild();
}

void DrumsView::updatePage()
{
    const QString trackName = track_ ? QString::fromStdString(track_->getName().toStdString())
                                     : QString();
    if (drumMachine()) {
        titleLabel_->setText(QString("Drum Machine: %1").arg(trackName));
        stack_->setCurrentIndex(1);
        return;
    }
    emptyLabel_->setText(track_ ? QString("'%1' has no Drum Machine.").arg(trackName)
                                : QString("Select a track to edit its Drum Machine."));
    loadButton_->setVisible(track_ != nullptr);
    stack_->setCurrentIndex(0);
}

// ── Building the view ───────────────────────────────────────────────────────

void DrumsView::scheduleRebuild()
{
    if (!rebuildTimer_.isActive())
        rebuildTimer_.start(0);
}

void DrumsView::rebuild()
{
    rebuildTimer_.stop();
    // Never pull a knob out from under a gesture; catch up when it ends.
    if (editor_->gestureActive()) {
        rebuildTimer_.start(kGestureRetryMs);
        return;
    }
    ++rebuildCount_;

    // Later: the old pads may be mid-event (a drop or a menu caused this).
    for (auto* pad : pads_) {
        pad->hide();
        pad->deleteLater();
    }
    pads_.clear();
    while (auto* item = gridLayout_->takeAt(0)) delete item;

    auto* dm = drumMachine();
    kit_ = dm ? dm->kit() : Drums::Kit{};
    auto sorted = kit_.pads;
    std::sort(sorted.begin(), sorted.end(),
              [](const Drums::Pad& a, const Drums::Pad& b) { return a.note < b.note; });

    // Warnings start "Pad <note>:", which is how the pads are marked.
    const QStringList warnings = dm ? dm->warnings() : QStringList();
    QMap<int, QStringList> padWarnings;
    static const QRegularExpression padPrefix("^Pad (\\d+):");
    for (const auto& w : warnings)
        if (const auto m = padPrefix.match(w); m.hasMatch())
            padWarnings[m.captured(1).toInt()] << w;

    for (int i = 0; i < sorted.size(); ++i) {
        auto* w = new DrumPadWidget(sorted[i], gridArea_);
        w->setWarning(padWarnings.value(sorted[i].note).join("\n"));
        connect(w, &DrumPadWidget::pressed, this, [this](int note) { selectPad(note); });
        connect(w, &DrumPadWidget::menuRequested, this, &DrumsView::showPadMenu);
        connect(w, &DrumPadWidget::filesDropped, this, &DrumsView::dropFiles);
        gridLayout_->addWidget(w, i / kPadColumns, i % kPadColumns);
        pads_.push_back(w);
    }

    warningBar_->setText(warnings.join("\n"));
    warningBar_->setVisible(!warnings.isEmpty());

    if (!kit_.padForNote(selectedNote_))
        selectedNote_ = sorted.isEmpty() ? -1 : sorted.first().note;
    updateSelection();
}

void DrumsView::updateSelection()
{
    for (auto* pad : pads_)
        pad->setSelected(pad->note() == selectedNote_);
    editor_->setPad(kit_.padForNote(selectedNote_));
}

DrumPadWidget* DrumsView::padWidget(int note) const
{
    for (auto* pad : pads_)
        if (pad->note() == note) return pad;
    return nullptr;
}

QString DrumsView::warningText() const
{
    return warningBar_->isVisibleTo(this) ? warningBar_->text() : QString();
}

void DrumsView::selectPad(int note, bool audition)
{
    if (!kit_.padForNote(note)) return;
    selectedNote_ = note;
    updateSelection();
    if (audition) auditionPad(note);
}

void DrumsView::auditionPad(int note)
{
    if (auto* dm = drumMachine()) dm->auditionNote(note, kAuditionVelocity);
}

// ── Edits ───────────────────────────────────────────────────────────────────

bool DrumsView::applyKit(const Drums::Kit& kit, const QString& undoName)
{
    auto* dm = drumMachine();
    if (!dm) return false;
    const auto errors = kit.validate();
    if (!errors.isEmpty()) {
        showError(errors.join(" "));
        return false;
    }
    lastError_.clear();
    dm->setKit(kit, toJuce(undoName));
    scheduleRebuild();
    emit editMgr_->editChanged();
    return true;
}

bool DrumsView::editPad(int note, const DrumPadEditor::Change& change, const QString& undoName)
{
    auto* dm = drumMachine();
    if (!dm) return false;
    auto kit = dm->kit();
    auto* pad = kit.padForNote(note);
    if (!pad) {
        showError(QString("Note %1 has no pad.").arg(note));
        return false;
    }
    const auto before = kit.toJson();
    change(*pad);
    if (kit.toJson() == before) return false;   // nothing changed: no empty undo step
    return applyKit(kit, undoName);
}

int DrumsView::nextFreeNote() const
{
    const auto kit = drumMachine() ? drumMachine()->kit() : kit_;
    return Drums::nextFreeDrumNote([&kit](int n) { return kit.padForNote(n) != nullptr; });
}

bool DrumsView::addPad()
{
    auto* dm = drumMachine();
    if (!dm) return false;
    const int note = nextFreeNote();
    if (note < 0) {
        showError("Every MIDI note already has a pad.");
        return false;
    }
    auto kit = dm->kit();
    kit.pads.append(Drums::defaultPadForNote(note));
    if (!applyKit(kit, "Add Pad")) return false;
    selectedNote_ = note;   // the rebuild selects it
    return true;
}

bool DrumsView::renamePad(int note, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    return editPad(note, [trimmed](Drums::Pad& p) { p.name = trimmed; }, "Rename Pad");
}

bool DrumsView::duplicatePad(int note, int toNote)
{
    auto* dm = drumMachine();
    if (!dm) return false;
    auto kit = dm->kit();
    const auto* pad = kit.padForNote(note);
    if (!pad) return false;
    if (toNote < 0 || toNote > 127) {
        showError(QString("Note %1 is outside MIDI's 0..127.").arg(toNote));
        return false;
    }
    if (const auto* taken = kit.padForNote(toNote)) {
        showError(QString("Note %1 already has a pad (%2).").arg(toNote).arg(taken->name));
        return false;
    }
    auto copy = *pad;
    copy.note = toNote;
    kit.pads.append(copy);
    if (!applyKit(kit, "Duplicate Pad")) return false;
    selectedNote_ = toNote;
    return true;
}

bool DrumsView::removePad(int note)
{
    auto* dm = drumMachine();
    if (!dm) return false;
    auto kit = dm->kit();
    const auto removed = std::remove_if(kit.pads.begin(), kit.pads.end(),
                                        [note](const Drums::Pad& p) { return p.note == note; });
    if (removed == kit.pads.end()) return false;
    kit.pads.erase(removed, kit.pads.end());
    return applyKit(kit, "Remove Pad");
}

bool DrumsView::resetPad(int note)
{
    const auto defaults = Drums::Kit::defaultKit();
    const auto* original = defaults.padForNote(note);
    if (!original) {
        showError(QString("The default kit has no pad on note %1.").arg(note));
        return false;
    }
    const auto pad = *original;
    return editPad(note, [pad](Drums::Pad& p) { p = pad; }, "Reset Pad");
}

bool DrumsView::isAudioFile(const QString& path)
{
    static const QStringList kExtensions = { "wav", "aiff", "aif", "flac", "ogg" };
    return kExtensions.contains(QFileInfo(path).suffix().toLower());
}

bool DrumsView::loadSample(int note, const QString& path)
{
    auto* dm = drumMachine();
    if (!dm) return false;
    if (!isAudioFile(path)) {
        showError(QString("'%1' isn't a sample the Drum Machine can play (wav, aiff, aif, flac, ogg).")
                      .arg(QFileInfo(path).fileName()));
        return false;
    }
    const QString file = QFileInfo(path).absoluteFilePath();
    if (note >= 0)
        return editPad(note, [file](Drums::Pad& p) {
            p.source = Drums::PadSource::Sample;
            p.sample = file;
            p.module.clear();
            p.params.clear();
        }, "Load Sample");

    const int free = nextFreeNote();
    if (free < 0) {
        showError("Every MIDI note already has a pad.");
        return false;
    }
    Drums::Pad pad;
    pad.note = free;
    pad.name = QFileInfo(file).completeBaseName();
    pad.source = Drums::PadSource::Sample;
    pad.sample = file;
    auto kit = dm->kit();
    kit.pads.append(pad);
    if (!applyKit(kit, "Add Sample Pad")) return false;
    selectedNote_ = free;
    return true;
}

bool DrumsView::dropFiles(int note, const QList<QUrl>& urls)
{
    if (urls.size() != 1 || !urls.first().isLocalFile()) {
        showError("Drop one audio file at a time (wav, aiff, aif, flac, ogg).");
        return false;
    }
    return loadSample(note, urls.first().toLocalFile());
}

void DrumsView::browseSample(int note)
{
    const auto* pad = kit_.padForNote(note);
    const QString start = pad && !pad->sample.isEmpty() ? QFileInfo(pad->sample).absolutePath() : QString();
    const QString path = QFileDialog::getOpenFileName(this, "Load Sample", start,
                                                      "Audio Files (*.wav *.aiff *.aif *.flac *.ogg)");
    if (path.isEmpty() || !loadSample(note, path))
        updateSelection();   // puts the source switch back
}

void DrumsView::showPadMenu(int note, const QPoint& globalPos)
{
    const auto* pad = kit_.padForNote(note);
    if (!pad) return;
    selectPad(note, false);
    const QString name = pad->name;

    QMenu menu;
    menu.setAccessibleName("Pad Menu");
    menu.addAction("Play", this, [this, note]() { auditionPad(note); });
    menu.addAction(QString::fromUtf8("Rename…"), this, [this, note, name]() {
        bool ok = false;
        const QString text = QInputDialog::getText(this, "Rename Pad", "Name:", QLineEdit::Normal, name, &ok);
        if (ok) renamePad(note, text);
    });
    menu.addAction(QString::fromUtf8("Duplicate to Note…"), this, [this, note]() {
        bool ok = false;
        const int to = QInputDialog::getInt(this, "Duplicate Pad", "Copy to MIDI note (0-127):",
                                            std::max(0, nextFreeNote()), 0, 127, 1, &ok);
        if (ok) duplicatePad(note, to);
    });
    auto* reset = menu.addAction("Reset to Default", this, [this, note]() { resetPad(note); });
    reset->setEnabled(Drums::Kit::defaultKit().padForNote(note) != nullptr);
    menu.addSeparator();
    menu.addAction("Remove", this, [this, note]() { removePad(note); });
    menu.exec(globalPos);
}

void DrumsView::showError(const QString& text)
{
    lastError_ = text;
    auto* host = errorLabel_->parentWidget();
    errorLabel_->setText(text);
    errorLabel_->setFixedWidth(std::min(480, std::max(200, host->width() - 24)));
    errorLabel_->adjustSize();
    errorLabel_->move(12, 12);
    errorLabel_->show();
    errorLabel_->raise();
    errorTimer_.start();
}

} // namespace OpenDaw
