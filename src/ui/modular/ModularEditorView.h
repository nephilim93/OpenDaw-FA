#pragma once

#include "ModulePanelItem.h"
#include "engine/EditManager.h"
#include "modular/Patch.h"
#include "ui/routing/CableItem.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
#include <QJsonArray>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QStackedWidget;
class QGraphicsRectItem;
class QGraphicsSimpleTextItem;

namespace OpenDaw {

class ModularSynthPlugin;

// The "Modular" tab: the selected track's Modular Synth patch as module
// panels and cables. Every edit goes through Modular::applyOps and
// ModularSynthPlugin::setPatch as one named undo step; the view itself is
// rebuilt from the plugin state whenever the patch changes (editor, AI,
// undo/redo).
class ModularEditorView : public QWidget, private juce::ValueTree::Listener {
    Q_OBJECT

public:
    explicit ModularEditorView(EditManager* editMgr, QWidget* parent = nullptr);
    ~ModularEditorView() override;

    void setTrack(te::AudioTrack* track);
    te::AudioTrack* track() const { return track_; }
    ModularSynthPlugin* synth() const;

    // The edit entry points the mouse handlers use. They return false (and
    // flash the error) when applyOps rejects the change.
    bool applyEdit(const QJsonArray& ops, const QString& undoName);
    bool connectPorts(const Modular::PortRef& from, const Modular::PortRef& to);
    bool disconnectCable(const Modular::Cable& cable);
    bool removeModule(const QString& id);
    bool addModule(const QString& type, QPointF scenePos);
    // Faust code modules: what the code dialog's Save does, one undo step each.
    // editModuleCode refuses code that would drop cables unless allowed.
    bool addCodeModule(const QString& code, QPointF scenePos);
    bool editModuleCode(const QString& id, const QString& code, bool allowDroppedCables = false);
    // Cables that new code for `id` would drop (ports it no longer has);
    // empty if none or the code doesn't compile.
    QVector<Modular::Cable> cablesDroppedBy(const QString& id, const QString& code) const;
    bool setParam(const QString& module, const QString& param, float value);

    // Introspection for tests and accessibility.
    QGraphicsScene* scene() const { return scene_; }
    int moduleItemCount() const { return int(panels_.size()); }
    int cableItemCount() const { return int(cables_.size()); }
    ModulePanelItem* panel(const QString& id) const;
    QString lastError() const { return lastError_; }
    double zoomLevel() const { return view_->transform().m11(); }

public slots:
    void rebuild();
    void scheduleRebuild();
    void zoomToFit();
    void zoomIn() { zoomBy(1.25); }
    void zoomOut() { zoomBy(1.0 / 1.25); }

protected:
    void showEvent(QShowEvent* event) override;

private:
    // id empty: a new module at scenePos, starting from the template.
    void openCodeDialog(const QString& id, QPointF scenePos);
    struct CableEntry {
        CableItem* item = nullptr;
        Modular::Cable cable;
    };

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& id) override;

    void refreshTarget();
    void attachToSynth(ModularSynthPlugin* synth);
    bool trackStillExists() const;
    void updatePage();
    void clearScene();
    void updateZones();
    void updateSceneRect();
    void updateWarnings();
    void showError(const QString& text);
    void commitMoves();
    QString uniqueModuleId(const QString& type) const;
    Modular::Section sectionAt(QPointF scenePos) const;

    bool eventFilter(QObject* watched, QEvent* event) override;
    ModuleJack* findJackAt(const QPointF& scenePos, bool wantInput, qreal radius) const;
    ModulePanelItem* panelAt(const QPointF& scenePos) const;
    CableEntry* cableAt(const QPointF& scenePos);
    void startCableDrag(ModuleJack* fromJack, const QPointF& scenePos,
                        std::optional<Modular::Cable> detached);
    void updateCableDrag(const QPointF& scenePos);
    void finishCableDrag(const QPointF& scenePos);
    void cancelCableDrag();
    void showModuleMenu(ModulePanelItem* panel, const QPoint& screenPos);
    void showCableMenu(const Modular::Cable& cable, const QPoint& screenPos);
    void showBackgroundMenu(const QPointF& scenePos, const QPoint& screenPos);
    void zoomBy(double factor);
    void startCablePhysics();
    void tickCablePhysics();

    EditManager* editMgr_;
    te::AudioTrack* track_ = nullptr;
    te::Plugin::Ptr plugin_;
    juce::ValueTree pluginState_;
    Modular::Patch patch_;

    QStackedWidget* stack_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsView* view_ = nullptr;

    std::vector<ModulePanelItem*> panels_;
    std::vector<CableEntry> cables_;
    QGraphicsRectItem* voiceZone_ = nullptr;
    QGraphicsRectItem* globalZone_ = nullptr;

    CableItem* dragCable_ = nullptr;
    ModuleJack* dragSourceJack_ = nullptr;
    ModuleJack* dragHoverJack_ = nullptr;
    std::optional<Modular::Cable> detachedCable_;

    bool fitOnNextBuild_ = true;

    // Zoom and position per Modular Synth (by its edit item id), so
    // switching tracks and back keeps the view instead of re-fitting.
    struct ViewState { QTransform transform; QPointF center; };
    QHash<quint64, ViewState> viewStates_;
    void saveViewState();
    bool restoreViewState();
    bool panning_ = false;
    QPoint panStartPos_;
    int panStartH_ = 0, panStartV_ = 0;

    QString lastError_;
    QTimer rebuildTimer_;
    QTimer physicsTimer_;
    QTimer errorTimer_;
};

} // namespace OpenDaw
