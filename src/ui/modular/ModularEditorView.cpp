#include "ModularEditorView.h"
#include "FaustCodeDialog.h"
#include "modular/FaustStarters.h"
#include <QNativeGestureEvent>
#include "engine/modular/ModularSynthPlugin.h"
#include "utils/ThemeManager.h"
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <map>

namespace OpenDaw {

namespace {

// ModularSynthPlugin keeps its patch JSON under this property of its state.
const juce::Identifier kPatchProperty("modularPatch");

constexpr qreal kZonePadding = 30.0;
constexpr qreal kZoneLabelSpace = 22.0;
constexpr qreal kPressRadius = 10.0;
constexpr qreal kDropRadius = 20.0;
constexpr int kErrorFlashMs = 4000;

juce::String toJuce(const QString& s)
{
    return juce::String::fromUTF8(s.toUtf8().constData());
}

QJsonObject cableOp(const char* op, const Modular::Cable& c)
{
    return QJsonObject{ { "op", op }, { "from", c.from.toString() }, { "to", c.to.toString() } };
}

QGraphicsRectItem* makeZone(QGraphicsScene* scene, const QString& name, const QColor& tint)
{
    auto* zone = scene->addRect(QRectF(), QPen(tint.darker(150), 1.0, Qt::DashLine),
                                QBrush(QColor(tint.red(), tint.green(), tint.blue(), 22)));
    zone->setZValue(-10);
    auto* label = new QGraphicsSimpleTextItem(name, zone);
    QFont f = label->font();
    f.setPixelSize(12);
    f.setBold(true);
    label->setFont(f);
    label->setBrush(tint.lighter(120));
    return zone;
}

} // namespace

ModularEditorView::ModularEditorView(EditManager* editMgr, QWidget* parent)
    : QWidget(parent), editMgr_(editMgr)
{
    setAccessibleName("Modular Editor");
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
    loadButton_ = new QPushButton("Load Modular Synth", emptyPage);
    loadButton_->setAccessibleName("Load Modular Synth");
    loadButton_->setFixedWidth(180);
    emptyLayout->addWidget(loadButton_, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    connect(loadButton_, &QPushButton::clicked, this, [this]() {
        if (!track_ || !trackStillExists()) return;
        editMgr_->setTrackModularSynth(*track_);
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
    header->addStretch();
    auto* zoomOutButton = new QPushButton(QString::fromUtf8("\u2212"), editorPage);
    zoomOutButton->setAccessibleName("Zoom Out");
    zoomOutButton->setToolTip("Zoom out (Ctrl+scroll or pinch also zoom)");
    zoomOutButton->setFixedWidth(28);
    connect(zoomOutButton, &QPushButton::clicked, this, &ModularEditorView::zoomOut);
    header->addWidget(zoomOutButton);
    auto* zoomInButton = new QPushButton("+", editorPage);
    zoomInButton->setAccessibleName("Zoom In");
    zoomInButton->setToolTip("Zoom in (Ctrl+scroll or pinch also zoom)");
    zoomInButton->setFixedWidth(28);
    connect(zoomInButton, &QPushButton::clicked, this, &ModularEditorView::zoomIn);
    header->addWidget(zoomInButton);
    auto* fitButton = new QPushButton("Zoom to Fit", editorPage);
    fitButton->setAccessibleName("Zoom to Fit");
    connect(fitButton, &QPushButton::clicked, this, &ModularEditorView::zoomToFit);
    header->addWidget(fitButton);
    editorLayout->addLayout(header);

    warningBar_ = new QWidget(editorPage);
    warningBar_->setAccessibleName("Patch Warnings");
    warningBar_->setObjectName("warningBar");
    warningBar_->setStyleSheet("#warningBar { background: #4a3f12; }"
                               "QLabel { color: #ffd866; font-size: 11px; }"
                               "QPushButton { font-size: 11px; padding: 1px 10px; }");
    auto* warningLayout = new QVBoxLayout(warningBar_);
    warningLayout->setContentsMargins(8, 3, 8, 3);
    warningLayout->setSpacing(2);
    warningBar_->hide();
    editorLayout->addWidget(warningBar_);

    scene_ = new QGraphicsScene(this);
    scene_->setItemIndexMethod(QGraphicsScene::NoIndex);
    scene_->setBackgroundBrush(theme.background.darker(110));

    view_ = new QGraphicsView(scene_, editorPage);
    view_->setAccessibleName("Modular Patch Canvas");
    view_->setRenderHint(QPainter::Antialiasing);
    view_->setDragMode(QGraphicsView::RubberBandDrag);
    view_->setFrameStyle(0);
    view_->setFocusPolicy(Qt::StrongFocus);
    view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view_->viewport()->installEventFilter(this);
    view_->installEventFilter(this);
    editorLayout->addWidget(view_, 1);

    // Floats over the canvas so a rejected edit doesn't shift the layout.
    errorLabel_ = new QLabel(view_);
    errorLabel_->setAccessibleName("Patch Error");
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
    connect(&rebuildTimer_, &QTimer::timeout, this, &ModularEditorView::rebuild);

    physicsTimer_.setInterval(16);
    connect(&physicsTimer_, &QTimer::timeout, this, &ModularEditorView::tickCablePhysics);

    connect(editMgr_, &EditManager::aboutToChangeEdit, this, [this]() { setTrack(nullptr); });
    connect(editMgr_, &EditManager::tracksChanged, this, &ModularEditorView::refreshTarget);
    connect(editMgr_, &EditManager::editChanged, this, &ModularEditorView::refreshTarget);

    updatePage();
}

ModularEditorView::~ModularEditorView()
{
    rebuildTimer_.stop();
    attachToSynth(nullptr);
    disconnect(editMgr_, nullptr, this, nullptr);
}

// ── Target track / plugin ───────────────────────────────────────────────────

void ModularEditorView::setTrack(te::AudioTrack* track)
{
    track_ = track;
    refreshTarget();
}

ModularSynthPlugin* ModularEditorView::synth() const
{
    return dynamic_cast<ModularSynthPlugin*>(plugin_.get());
}

bool ModularEditorView::trackStillExists() const
{
    if (!track_ || !editMgr_->edit()) return false;
    return te::getAudioTracks(*editMgr_->edit()).contains(track_);
}

void ModularEditorView::refreshTarget()
{
    if (track_ && !trackStillExists())
        track_ = nullptr;

    auto* found = track_ ? dynamic_cast<ModularSynthPlugin*>(editMgr_->getTrackInstrument(track_))
                         : nullptr;
    if (found != synth()) {
        attachToSynth(found);
        rebuild();
    }
    updatePage();
}

void ModularEditorView::attachToSynth(ModularSynthPlugin* synth)
{
    if (pluginState_.isValid())
        pluginState_.removeListener(this);
    // Our scene may still point at the old plugin's jacks mid-drag.
    cancelCableDrag();
    saveViewState();
    plugin_ = synth;
    fitOnNextBuild_ = true;
    pluginState_ = synth ? synth->state : juce::ValueTree();
    if (pluginState_.isValid())
        pluginState_.addListener(this);
}

void ModularEditorView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& id)
{
    // Deferred: the plugin parses the new JSON in its own listener, which
    // may run after this one.
    if (tree == pluginState_ && id == kPatchProperty)
        scheduleRebuild();
}

void ModularEditorView::updatePage()
{
    const QString trackName = track_ ? QString::fromStdString(track_->getName().toStdString())
                                     : QString();
    if (synth()) {
        titleLabel_->setText(QString("Modular Synth: %1").arg(trackName));
        stack_->setCurrentIndex(1);
        return;
    }
    emptyLabel_->setText(track_ ? QString("'%1' has no Modular Synth.").arg(trackName)
                                : QString("Select a track to edit its Modular Synth."));
    loadButton_->setVisible(track_ != nullptr);
    stack_->setCurrentIndex(0);
}

// ── Building the scene ──────────────────────────────────────────────────────

void ModularEditorView::scheduleRebuild()
{
    if (!rebuildTimer_.isActive())
        rebuildTimer_.start(0);
}

void ModularEditorView::clearScene()
{
    cancelCableDrag();
    physicsTimer_.stop();
    cables_.clear();
    panels_.clear();
    voiceZone_ = globalZone_ = nullptr;
    scene_->clear();
}

void ModularEditorView::rebuild()
{
    rebuildTimer_.stop();

    // Never pull items out from under a gesture; catch up when it ends.
    if (dragCable_ || panning_ || scene_->mouseGrabberItem()) {
        rebuildTimer_.start(100);
        return;
    }

    // Keep the same spot in view across a rebuild: the scene rect changes
    // with the patch, so raw scrollbar values would drift.
    const QPointF center = view_->mapToScene(view_->viewport()->rect().center());

    clearScene();
    auto* s = synth();
    patch_ = s ? s->patch() : Modular::Patch{};
    updateWarnings();
    if (!s) return;

    voiceZone_ = makeZone(scene_, "VOICE (per note)", QColor(0, 150, 136));
    globalZone_ = makeZone(scene_, "GLOBAL (shared)", QColor(126, 87, 194));

    std::map<QString, ModulePanelItem*> byId;
    for (const auto& m : patch_.modules) {
        const auto* def = Modular::moduleDef(m);
        if (!def) continue;
        auto* panel = new ModulePanelItem(m, *def, patch_);
        panel->setPos(m.x, m.y);
        scene_->addItem(panel);
        panels_.push_back(panel);
        byId[m.id] = panel;

        connect(panel, &ModulePanelItem::moved, this, [this]() {
            for (auto& c : cables_) c.item->updatePath();
            updateZones();
            startCablePhysics();
        });
        connect(panel, &ModulePanelItem::dragFinished, this, &ModularEditorView::commitMoves);
        const QString id = m.id;
        connect(panel, &ModulePanelItem::paramCommitted, this,
                [this, id](const QString& param, float value) { setParam(id, param, value); });
    }

    for (const auto& c : patch_.cables) {
        auto src = byId.find(c.from.module), dst = byId.find(c.to.module);
        if (src == byId.end() || dst == byId.end()) continue;
        auto* from = src->second->findJack(c.from.port, false);
        auto* to = dst->second->findJack(c.to.port, true);
        if (!from || !to) continue;
        const QColor color = ModulePanelItem::kindColor(from->kind());
        auto* item = new CableItem(from, to, color);
        scene_->addItem(item);
        from->setConnected(true, color);
        to->setConnected(true, color);
        cables_.push_back({ item, c });
    }

    updateZones();
    updateSceneRect();
    // A hidden view has no size to fit to; showEvent fits it later.
    if (fitOnNextBuild_ && view_->isVisible()) {
        fitOnNextBuild_ = false;
        if (!restoreViewState())
            zoomToFit();
    } else {
        view_->centerOn(center);
    }
}

void ModularEditorView::updateZones()
{
    if (!voiceZone_ || !globalZone_) return;

    QRectF voice, global;
    for (auto* p : panels_) {
        const QRectF r = p->sceneBoundingRect();
        (p->section() == Modular::Section::Voice ? voice : global) |= r;
    }
    // Empty sections still get a zone, so there is somewhere to add modules.
    if (voice.isNull()) voice = QRectF(40, 40, 600, 300);
    if (global.isNull()) global = QRectF(voice.left(), std::max(460.0, voice.bottom() + 80.0), 600, 160);

    const qreal left = std::min(voice.left(), global.left()) - kZonePadding;
    const qreal right = std::max(voice.right(), global.right()) + kZonePadding;
    qreal voiceBottom = voice.bottom() + kZonePadding;
    qreal globalTop = global.top() - kZonePadding - kZoneLabelSpace;
    // Sections closer than the padding share one border, kept just above
    // the global label.
    if (voiceBottom > globalTop)
        voiceBottom = globalTop = std::max(voice.bottom(), global.top() - kZoneLabelSpace);
    auto place = [](QGraphicsRectItem* zone, const QRectF& r) {
        zone->setRect(r);
        if (auto* label = zone->childItems().value(0))
            label->setPos(r.topLeft() + QPointF(10, 4));
    };
    place(voiceZone_, QRectF(QPointF(left, voice.top() - kZonePadding - kZoneLabelSpace),
                             QPointF(right, voiceBottom)));
    place(globalZone_, QRectF(QPointF(left, globalTop),
                              QPointF(right, global.bottom() + kZonePadding)));
}

void ModularEditorView::updateSceneRect()
{
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-300, -200, 300, 200));
}

void ModularEditorView::updateWarnings()
{
    issues_ = synth() ? patch_.issues() : QVector<Modular::PatchIssue>();
    auto* layout = warningBar_->layout();
    while (auto* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < issues_.size(); ++i) {
        const auto& issue = issues_[i];
        auto* row = new QWidget(warningBar_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* text = new QLabel(issue.text, row);
        text->setWordWrap(true);
        rowLayout->addWidget(text, 1);
        if (!issue.fixOps.isEmpty()) {
            auto* fix = new QPushButton("Fix", row);
            fix->setAccessibleName("Fix: " + issue.fixLabel);
            fix->setToolTip(issue.fixLabel);
            connect(fix, &QPushButton::clicked, this, [this, i]() { applyIssueFix(i); });
            rowLayout->addWidget(fix);
        }
        layout->addWidget(row);
    }
    warningBar_->setVisible(!issues_.isEmpty());
}

void ModularEditorView::zoomToFit()
{
    if (!voiceZone_) return;
    const QRectF r = voiceZone_->sceneBoundingRect().united(globalZone_->sceneBoundingRect());
    view_->fitInView(r.adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
    // Past 1:1 knobs look blurry and nothing gains from it.
    if (view_->transform().m11() > 1.0)
        view_->setTransform(QTransform());
    view_->centerOn(r.center());
}

void ModularEditorView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (fitOnNextBuild_ && voiceZone_) {
        fitOnNextBuild_ = false;
        QTimer::singleShot(0, this, [this]() {
            if (!restoreViewState())
                zoomToFit();
        });
    }
}

void ModularEditorView::saveViewState()
{
    if (!plugin_ || !view_->isVisible()) return;
    viewStates_[plugin_->itemID.getRawID()] = {
        view_->transform(), view_->mapToScene(view_->viewport()->rect().center()) };
}

bool ModularEditorView::restoreViewState()
{
    if (!plugin_) return false;
    const auto it = viewStates_.constFind(plugin_->itemID.getRawID());
    if (it == viewStates_.constEnd()) return false;
    view_->setTransform(it->transform);
    view_->centerOn(it->center);
    return true;
}

void ModularEditorView::zoomBy(double factor)
{
    const double scale = view_->transform().m11() * factor;
    if (scale < 0.2 || scale > 3.0) return;
    view_->scale(factor, factor);
}

ModulePanelItem* ModularEditorView::panel(const QString& id) const
{
    for (auto* p : panels_)
        if (p->moduleId() == id) return p;
    return nullptr;
}

// ── Edits ───────────────────────────────────────────────────────────────────

bool ModularEditorView::applyEdit(const QJsonArray& ops, const QString& undoName)
{
    auto* s = synth();
    if (!s || ops.isEmpty()) return false;

    auto patch = s->patch();
    const QString error = Modular::applyOps(patch, ops);
    if (!error.isEmpty()) {
        showError(error);
        return false;
    }
    lastError_.clear();
    s->setPatch(patch, toJuce(undoName));
    scheduleRebuild();
    emit editMgr_->editChanged();
    return true;
}

bool ModularEditorView::connectPorts(const Modular::PortRef& from, const Modular::PortRef& to)
{
    return applyEdit({ cableOp("connect", { from, to }) }, "Connect Cable");
}

bool ModularEditorView::disconnectCable(const Modular::Cable& cable)
{
    return applyEdit({ cableOp("disconnect", cable) }, "Disconnect Cable");
}

bool ModularEditorView::removeModule(const QString& id)
{
    return applyEdit({ QJsonObject{ { "op", "remove_module" }, { "id", id } } }, "Remove Module");
}

bool ModularEditorView::addModule(const QString& type, QPointF scenePos)
{
    const auto* def = Modular::findModule(type.toStdString());
    if (!def) return false;
    const auto section = def->section == Modular::Section::Both ? sectionAt(scenePos) : def->section;
    return applyEdit({ QJsonObject{
                         { "op", "add_module" }, { "id", uniqueModuleId(type) }, { "type", type },
                         { "section", Modular::sectionName(section) },
                         { "x", std::round(scenePos.x()) }, { "y", std::round(scenePos.y()) } } },
                     "Add Module");
}

bool ModularEditorView::addCodeModule(const QString& code, QPointF scenePos, const QString& idBase)
{
    return applyEdit({ QJsonObject{
                         { "op", "write_module" }, { "id", uniqueModuleId(idBase) }, { "code", code },
                         { "section", Modular::sectionName(sectionAt(scenePos)) },
                         { "x", std::round(scenePos.x()) }, { "y", std::round(scenePos.y()) } } },
                     "Add Code Module");
}

QVector<Modular::Cable> ModularEditorView::cablesDroppedBy(const QString& id, const QString& code) const
{
    QVector<Modular::Cable> dropped;
    const auto result = Modular::compileCodeModule(id.toStdString(), code.toStdString());
    if (!result.def) return dropped;
    for (const auto& c : patch_.cables)
        if ((c.from.module == id && result.def->outputIndex(c.from.port.toStdString()) < 0)
            || (c.to.module == id && result.def->inputIndex(c.to.port.toStdString()) < 0))
            dropped.append(c);
    return dropped;
}

bool ModularEditorView::editModuleCode(const QString& id, const QString& code, bool allowDroppedCables)
{
    if (!allowDroppedCables) {
        const auto dropped = cablesDroppedBy(id, code);
        if (!dropped.isEmpty()) {
            QStringList names;
            for (const auto& c : dropped) names << c.from.toString() + " → " + c.to.toString();
            showError(QString("The new code has no port for %1.").arg(names.join(", ")));
            return false;
        }
    }
    return applyEdit({ QJsonObject{ { "op", "write_module" }, { "id", id }, { "code", code } } },
                     "Edit Code");
}

void ModularEditorView::openCodeDialog(const QString& id, QPointF scenePos)
{
    const auto* m = patch_.find(id);
    const bool adding = !m;
    auto* dialog = new FaustCodeDialog(adding ? uniqueModuleId("code") : id,
                                       adding ? FaustCodeDialog::templateCode() : m->code,
                                       {}, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<FaustCodeDialog> guard(dialog);
    auto save = [this, id, adding, scenePos, guard](const QString& code) -> QString {
        if (adding) return addCodeModule(code, scenePos) ? QString() : lastError();
        if (!synth() || !patch_.find(id)) return "This module no longer exists.";
        const auto dropped = cablesDroppedBy(id, code);
        bool allow = false;
        if (!dropped.isEmpty()) {
            QStringList names;
            for (const auto& c : dropped) names << c.from.toString() + " → " + c.to.toString();
            const auto answer = QMessageBox::question(
                guard, "Remove Cables?",
                QString("The new code has no port for these cables, so saving removes them:\n%1")
                    .arg(names.join("\n")));
            if (answer != QMessageBox::Yes) return "Not saved: the cables were kept.";
            allow = true;
        }
        return editModuleCode(id, code, allow) ? QString() : lastError();
    };
    dialog->setSaveFunction(save);
    dialog->open();
}

bool ModularEditorView::applyIssueFix(int index)
{
    if (index < 0 || index >= issues_.size() || issues_[index].fixOps.isEmpty()) return false;
    const auto issue = issues_[index];   // applyEdit rebuilds issues_
    return applyEdit(issue.fixOps, "Fix: " + issue.fixLabel);
}

bool ModularEditorView::setParam(const QString& module, const QString& param, float value)
{
    return applyEdit({ QJsonObject{ { "op", "set_param" }, { "module", module },
                                    { "param", param }, { "value", double(value) } } },
                     "Change Parameter");
}

void ModularEditorView::commitMoves()
{
    // A group drag moves every selected panel: one undo step for all.
    QJsonArray ops;
    for (auto* p : panels_) {
        const auto* m = patch_.find(p->moduleId());
        if (!m) continue;
        const QPointF pos = p->pos();
        if (std::abs(pos.x() - m->x) < 0.5 && std::abs(pos.y() - m->y) < 0.5) continue;
        ops.append(QJsonObject{ { "op", "move_module" }, { "id", p->moduleId() },
                                { "x", std::round(pos.x()) }, { "y", std::round(pos.y()) } });
    }
    if (!ops.isEmpty())
        applyEdit(ops, ops.size() == 1 ? "Move Module" : "Move Modules");
}

void ModularEditorView::showError(const QString& text)
{
    lastError_ = text;
    errorLabel_->setText(text);
    errorLabel_->setFixedWidth(std::min(480, std::max(200, view_->width() - 24)));
    errorLabel_->adjustSize();
    errorLabel_->move(12, 12);
    errorLabel_->show();
    errorLabel_->raise();
    errorTimer_.start();
}

QString ModularEditorView::uniqueModuleId(const QString& type) const
{
    for (int n = 1;; ++n) {
        const QString id = type + QString::number(n);
        if (!patch_.find(id)) return id;
    }
}

Modular::Section ModularEditorView::sectionAt(QPointF scenePos) const
{
    if (!voiceZone_ || !globalZone_) return Modular::Section::Voice;
    const qreal split = (voiceZone_->rect().bottom() + globalZone_->rect().top()) / 2.0;
    return scenePos.y() > split ? Modular::Section::Global : Modular::Section::Voice;
}

// ── Mouse and keyboard ──────────────────────────────────────────────────────

ModuleJack* ModularEditorView::findJackAt(const QPointF& scenePos, bool wantInput, qreal radius) const
{
    ModuleJack* closest = nullptr;
    qreal best = radius;
    for (auto* p : panels_)
        for (auto* j : wantInput ? p->inputJacks() : p->outputJacks()) {
            const qreal d = QLineF(j->sceneCenterPos(), scenePos).length();
            if (d < best) { best = d; closest = j; }
        }
    return closest;
}

ModulePanelItem* ModularEditorView::panelAt(const QPointF& scenePos) const
{
    for (auto* item : scene_->items(scenePos))
        for (auto* i = item; i; i = i->parentItem())
            if (auto* p = dynamic_cast<ModulePanelItem*>(i)) return p;
    return nullptr;
}

ModularEditorView::CableEntry* ModularEditorView::cableAt(const QPointF& scenePos)
{
    for (auto& c : cables_)
        if (c.item->isVisible() && c.item->shape().contains(scenePos)) return &c;
    return nullptr;
}

bool ModularEditorView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == view_->viewport()) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            const QPointF scenePos = view_->mapToScene(me->pos());

            if (me->button() == Qt::RightButton) {
                const QPoint screen = me->globalPosition().toPoint();
                if (auto* c = cableAt(scenePos)) showCableMenu(c->cable, screen);
                else if (auto* p = panelAt(scenePos)) showModuleMenu(p, screen);
                else showBackgroundMenu(scenePos, screen);
                return true;
            }
            if (me->button() == Qt::MiddleButton) {
                panning_ = true;
                panStartPos_ = me->pos();
                panStartH_ = view_->horizontalScrollBar()->value();
                panStartV_ = view_->verticalScrollBar()->value();
                view_->viewport()->setCursor(Qt::ClosedHandCursor);
                return true;
            }
            if (me->button() == Qt::LeftButton) {
                // Outputs start a new cable (they can fan out); a plugged
                // input picks up its cable's end.
                if (auto* out = findJackAt(scenePos, false, kPressRadius)) {
                    startCableDrag(out, scenePos, std::nullopt);
                    return true;
                }
                if (auto* in = findJackAt(scenePos, true, kPressRadius)) {
                    for (auto it = cables_.rbegin(); it != cables_.rend(); ++it) {
                        if (!(it->cable.to == in->port())) continue;
                        auto* src = dynamic_cast<ModuleJack*>(it->item->sourceJack());
                        if (!src) break;
                        it->item->hide();
                        startCableDrag(src, scenePos, it->cable);
                        return true;
                    }
                }
            }
        }
        if (type == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (dragCable_) {
                updateCableDrag(view_->mapToScene(me->pos()));
                return true;
            }
            if (panning_) {
                const QPoint delta = me->pos() - panStartPos_;
                view_->horizontalScrollBar()->setValue(panStartH_ - delta.x());
                view_->verticalScrollBar()->setValue(panStartV_ - delta.y());
                return true;
            }
        }
        if (type == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && dragCable_) {
                finishCableDrag(view_->mapToScene(me->pos()));
                return true;
            }
            if (me->button() == Qt::MiddleButton && panning_) {
                panning_ = false;
                view_->viewport()->unsetCursor();
                return true;
            }
        }
        if (type == QEvent::NativeGesture) {
            auto* ge = static_cast<QNativeGestureEvent*>(event);
            if (ge->gestureType() == Qt::ZoomNativeGesture) {
                zoomBy(1.0 + ge->value());
                return true;
            }
        }
        if (type == QEvent::Wheel) {
            auto* we = static_cast<QWheelEvent*>(event);
            if (we->modifiers() & Qt::ControlModifier) {
                zoomBy(we->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
                return true;
            }
        }
    }

    if (watched == view_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape && dragCable_) {
            cancelCableDrag();
            return true;
        }
        // Only when the canvas itself has focus, not a dropdown inside it.
        if ((ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) && !scene_->focusItem()) {
            QJsonArray ops;
            for (auto* p : panels_)
                if (p->isSelected())
                    ops.append(QJsonObject{ { "op", "remove_module" }, { "id", p->moduleId() } });
            if (!ops.isEmpty())
                applyEdit(ops, ops.size() == 1 ? "Remove Module" : "Remove Modules");
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ModularEditorView::startCableDrag(ModuleJack* fromJack, const QPointF& scenePos,
                                       std::optional<Modular::Cable> detached)
{
    dragSourceJack_ = fromJack;
    detachedCable_ = detached;
    dragCable_ = new CableItem(fromJack, nullptr, ModulePanelItem::kindColor(fromJack->kind()));
    dragCable_->setDangling(true);
    dragCable_->setDanglingEnd(scenePos);
    scene_->addItem(dragCable_);
}

void ModularEditorView::updateCableDrag(const QPointF& scenePos)
{
    if (!dragCable_) return;
    dragCable_->setDanglingEnd(scenePos);
    startCablePhysics();

    auto* target = findJackAt(scenePos, true, kDropRadius);
    if (target != dragHoverJack_) {
        if (dragHoverJack_) dragHoverJack_->setHighlighted(false);
        dragHoverJack_ = target;
        if (dragHoverJack_) dragHoverJack_->setHighlighted(true);
    }
}

void ModularEditorView::finishCableDrag(const QPointF& scenePos)
{
    if (!dragCable_ || !dragSourceJack_) {
        cancelCableDrag();
        return;
    }
    const Modular::PortRef from = dragSourceJack_->port();
    auto* target = findJackAt(scenePos, true, kDropRadius);
    const auto detached = detachedCable_;

    bool changed = false;
    if (detached) {
        if (!target)
            changed = disconnectCable(*detached);
        else if (!(target->port() == detached->to))
            changed = applyEdit({ cableOp("disconnect", *detached),
                                  cableOp("connect", { from, target->port() }) },
                                "Move Cable");
    } else if (target) {
        connectPorts(from, target->port());
    }

    cancelCableDrag();
    // A picked-up cable that stayed put (or was refused) goes back in place
    // until the rebuild replaces the scene.
    if (detached && !changed)
        for (auto& c : cables_)
            if (c.cable == *detached) c.item->show();
}

void ModularEditorView::cancelCableDrag()
{
    if (dragHoverJack_) {
        dragHoverJack_->setHighlighted(false);
        dragHoverJack_ = nullptr;
    }
    if (dragCable_) {
        scene_->removeItem(dragCable_);
        delete dragCable_;
        dragCable_ = nullptr;
    }
    if (detachedCable_)
        for (auto& c : cables_)
            if (c.cable == *detachedCable_) c.item->show();
    detachedCable_.reset();
    dragSourceJack_ = nullptr;
}

void ModularEditorView::startCablePhysics()
{
    if (!physicsTimer_.isActive())
        physicsTimer_.start();
}

void ModularEditorView::tickCablePhysics()
{
    constexpr qreal dt = 0.016;
    bool anyMoving = false;
    for (auto& c : cables_)
        if (c.item->stepPhysics(dt)) anyMoving = true;
    if (dragCable_ && dragCable_->stepPhysics(dt))
        anyMoving = true;
    if (!anyMoving)
        physicsTimer_.stop();
}

// ── Context menus ───────────────────────────────────────────────────────────

void ModularEditorView::showModuleMenu(ModulePanelItem* panel, const QPoint& screenPos)
{
    const QString id = panel->moduleId();
    QMenu menu(this);
    menu.setAccessibleName("Module Context Menu");
    if (const auto* m = patch_.find(id); m && Modular::isCodeModule(*m))
        menu.addAction("Edit Code…", this, [this, id]() { openCodeDialog(id, {}); });
    menu.addAction(QString("Remove '%1'").arg(id), this, [this, id]() { removeModule(id); });
    menu.exec(screenPos);
}

void ModularEditorView::showCableMenu(const Modular::Cable& cable, const QPoint& screenPos)
{
    QMenu menu(this);
    menu.setAccessibleName("Cable Context Menu");
    menu.addAction(QString("Disconnect %1 → %2").arg(cable.from.toString(), cable.to.toString()),
                   this, [this, cable]() { disconnectCable(cable); });
    menu.exec(screenPos);
}

void ModularEditorView::showBackgroundMenu(const QPointF& scenePos, const QPoint& screenPos)
{
    if (!synth()) return;
    QMenu menu(this);
    menu.setAccessibleName("Add Module Menu");
    auto* add = menu.addMenu("Add Module");
    add->setToolTipsVisible(true);
    const auto here = sectionAt(scenePos);
    for (const auto category : Modular::kAllCategories) {
        QMenu* family = nullptr;
        for (const auto& def : Modular::allModules()) {
            // The placeholder is portless; code modules are added with their code.
            if (def.category != category || def.type == Modular::kCodeModuleType) continue;
            if (!family) {
                family = add->addMenu(Modular::categoryLabel(category));
                family->setToolTipsVisible(true);
            }
            // Say where a module will go when that isn't the zone clicked in.
            const QString type = QString::fromStdString(def.type);
            QString label = type;
            if (!def.allowedIn(here))
                label += def.section == Modular::Section::Voice ? "  (Voice)" : "  (Global)";
            auto* action = family->addAction(label, this, [this, type, scenePos]() { addModule(type, scenePos); });
            action->setToolTip(QString::fromStdString(def.description));
        }
        if (category == Modular::Category::Code && Modular::codeModulesAvailable()) {
            auto* code = add->addMenu(Modular::categoryLabel(category));
            code->setToolTipsVisible(true);
            code->addAction("New Faust Module…", this,
                            [this, scenePos]() { openCodeDialog({}, scenePos); });
            code->addSeparator();
            for (const auto& starter : Modular::codeStarters()) {
                const QString name = starter.name, source = starter.code;
                auto* action = code->addAction(name, this, [this, name, source, scenePos]() {
                    addCodeModule(source, scenePos, name);
                });
                action->setToolTip(QString(starter.description) + "\nRight-click it, then Edit Code… to change it.");
            }
        }
    }
    menu.exec(screenPos);
}

} // namespace OpenDaw
