#pragma once

#include "modular/Patch.h"
#include "ui/routing/RoutingNode.h"
#include <QGraphicsObject>
#include <QPointer>
#include <QTimer>
#include <vector>

class QComboBox;

namespace OpenDaw {

class RotaryKnob;

// A jack on a module panel, tied to one port of the patch.
class ModuleJack : public JackItem {
public:
    ModuleJack(bool isInput, int index, const Modular::PortRef& port,
               Modular::PortKind kind, QGraphicsItem* parent);

    const Modular::PortRef& port() const { return port_; }
    Modular::PortKind kind() const { return kind_; }

private:
    Modular::PortRef port_;
    Modular::PortKind kind_;
};

// One module of the patch: title, input jacks on the left, outputs on the
// right, a knob per numeric parameter and a dropdown per choice parameter.
// It never edits the patch itself; it reports gestures to the editor.
class ModulePanelItem : public QGraphicsObject {
    Q_OBJECT

public:
    ModulePanelItem(const Modular::PatchModule& module, const Modular::ModuleDef& def,
                    const Modular::Patch& patch, QGraphicsItem* parent = nullptr);

    QString moduleId() const { return moduleId_; }
    Modular::Section section() const { return section_; }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

    const std::vector<ModuleJack*>& inputJacks() const { return inputJacks_; }
    const std::vector<ModuleJack*>& outputJacks() const { return outputJacks_; }
    ModuleJack* findJack(const QString& port, bool input) const;
    RotaryKnob* knob(const QString& param) const;

    static QColor kindColor(Modular::PortKind kind);

    static constexpr qreal WIDTH = 172.0;
    static constexpr qreal HEADER_HEIGHT = 24.0;
    static constexpr qreal JACK_ROW_HEIGHT = 18.0;
    static constexpr qreal KNOB_WIDTH = 56.0;
    static constexpr qreal KNOB_HEIGHT = 50.0;
    static constexpr qreal CHOICE_ROW_HEIGHT = 22.0;

signals:
    void moved();
    // Released after being dragged somewhere else.
    void dragFinished();
    // A finished knob gesture or dropdown pick; one undo step each.
    void paramCommitted(const QString& param, float value);

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct KnobBinding {
        QPointer<RotaryKnob> knob;
        QString param;
        Modular::ParamDef def;
        float committed = 0.0f;
    };

    void addKnob(const Modular::ParamDef& def, float value, QPointF pos);
    void addChoice(const Modular::ParamDef& def, int index, qreal y);
    KnobBinding* bindingFor(QObject* knob);
    void commitKnob(KnobBinding& binding);

    QString moduleId_;
    QString type_;
    Modular::Section section_;
    qreal height_ = 0.0;
    int jackRows_ = 0;
    QStringList inputLabels_, outputLabels_;
    QStringList choiceLabels_;
    std::vector<qreal> choiceRowsY_;

    std::vector<ModuleJack*> inputJacks_;
    std::vector<ModuleJack*> outputJacks_;
    std::vector<KnobBinding> knobs_;
    KnobBinding* wheelBinding_ = nullptr;
    QTimer wheelCommitTimer_;
    QPointF pressPos_;
};

} // namespace OpenDaw
