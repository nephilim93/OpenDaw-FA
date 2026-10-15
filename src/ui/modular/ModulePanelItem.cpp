#include "ModulePanelItem.h"
#include "ui/controls/RotaryKnob.h"
#include "utils/ThemeManager.h"
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneMouseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <cmath>

namespace OpenDaw {

namespace {

constexpr int kKnobsPerRow = 3;
constexpr qreal kChoiceLabelWidth = 64.0;
constexpr int kWheelCommitMs = 400;

// Frequencies get a log knob so the musical range isn't squeezed into the
// first few degrees of travel.
bool isLogParam(const Modular::ParamDef& def)
{
    return def.unit == "Hz" && def.min > 0.0f && def.max > def.min;
}

double toKnob(const Modular::ParamDef& def, float value)
{
    if (def.max <= def.min) return 0.0;
    if (isLogParam(def))
        return std::log(std::max(value, def.min) / def.min) / std::log(def.max / def.min);
    return (value - def.min) / (def.max - def.min);
}

float fromKnob(const Modular::ParamDef& def, double n)
{
    n = std::clamp(n, 0.0, 1.0);
    if (isLogParam(def))
        return float(def.min * std::pow(double(def.max / def.min), n));
    return float(def.min + n * (def.max - def.min));
}

QString displayName(const std::string& name)
{
    return QString::fromStdString(name).replace('_', ' ');
}

QString formatValue(const Modular::ParamDef& def, float v)
{
    const float a = std::abs(v);
    const int decimals = a >= 100.0f ? 0 : a >= 10.0f ? 1 : 2;
    QString text = QString::number(double(v), 'f', decimals);
    if (!def.unit.empty()) text += " " + QString::fromStdString(def.unit);
    return text;
}

QString tooltipFor(const Modular::ParamDef& def, float v)
{
    QString tip = displayName(def.name) + ": " + formatValue(def, v);
    if (!def.description.empty()) tip += "\n" + QString::fromStdString(def.description);
    return tip;
}

} // namespace

// ── ModuleJack ──────────────────────────────────────────────────────────────

ModuleJack::ModuleJack(bool isInput, int index, const Modular::PortRef& port,
                       Modular::PortKind kind, QGraphicsItem* parent)
    : JackItem(isInput, index, parent), port_(port), kind_(kind)
{
    setRingColor(ModulePanelItem::kindColor(kind));
}

// ── ModulePanelItem ─────────────────────────────────────────────────────────

QColor ModulePanelItem::kindColor(Modular::PortKind kind)
{
    switch (kind) {
    case Modular::PortKind::Audio: return QColor(240, 160, 60);
    case Modular::PortKind::CV:    return QColor(90, 170, 255);
    case Modular::PortKind::Gate:  return QColor(235, 90, 95);
    case Modular::PortKind::Pitch: return QColor(120, 210, 120);
    }
    return QColor(Qt::gray);
}

ModulePanelItem::ModulePanelItem(const Modular::PatchModule& module,
                                 const Modular::ModuleDef& def,
                                 const Modular::Patch& patch, QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , moduleId_(module.id), type_(module.type), section_(module.section)
{
    setFlag(QGraphicsItem::ItemIsMovable, true);
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    setZValue(1);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QString::fromStdString(def.description));

    jackRows_ = int(std::max(def.inputs.size(), def.outputs.size()));
    auto jackY = [](int row) { return HEADER_HEIGHT + JACK_ROW_HEIGHT * (row + 0.5) + 3.0; };

    for (int i = 0; i < int(def.inputs.size()); ++i) {
        const auto& p = def.inputs[size_t(i)];
        auto* jack = new ModuleJack(true, i, { moduleId_, QString::fromStdString(p.name) }, p.kind, this);
        jack->setPos(0.0, jackY(i));
        jack->setToolTip(QString("%1 (%2 in)%3").arg(QString::fromStdString(p.name),
                         Modular::portKindName(p.kind),
                         p.description.empty() ? QString() : "\n" + QString::fromStdString(p.description)));
        inputJacks_.push_back(jack);
        inputLabels_ << QString::fromStdString(p.name);
    }
    for (int i = 0; i < int(def.outputs.size()); ++i) {
        const auto& p = def.outputs[size_t(i)];
        auto* jack = new ModuleJack(false, i, { moduleId_, QString::fromStdString(p.name) }, p.kind, this);
        jack->setPos(WIDTH, jackY(i));
        jack->setToolTip(QString("%1 (%2 out)%3").arg(QString::fromStdString(p.name),
                         Modular::portKindName(p.kind),
                         p.description.empty() ? QString() : "\n" + QString::fromStdString(p.description)));
        outputJacks_.push_back(jack);
        outputLabels_ << QString::fromStdString(p.name);
    }

    qreal y = HEADER_HEIGHT + JACK_ROW_HEIGHT * jackRows_ + 6.0;

    // Bindings are referenced by pointer from the wheel timer: no reallocation.
    knobs_.reserve(def.params.size());
    int knobIndex = 0;
    for (int i = 0; i < int(def.params.size()); ++i) {
        const auto& pd = def.params[size_t(i)];
        if (!pd.choices.empty()) continue;
        const int col = knobIndex % kKnobsPerRow, row = knobIndex / kKnobsPerRow;
        const qreal x = (WIDTH - kKnobsPerRow * KNOB_WIDTH) / 2.0 + col * KNOB_WIDTH;
        addKnob(pd, patch.paramValue(module, i), QPointF(x, y + row * KNOB_HEIGHT));
        ++knobIndex;
    }
    y += KNOB_HEIGHT * ((knobIndex + kKnobsPerRow - 1) / kKnobsPerRow);

    for (int i = 0; i < int(def.params.size()); ++i) {
        const auto& pd = def.params[size_t(i)];
        if (pd.choices.empty()) continue;
        addChoice(pd, int(std::lround(patch.paramValue(module, i))), y);
        y += CHOICE_ROW_HEIGHT;
    }
    height_ = y + 4.0;

    wheelCommitTimer_.setSingleShot(true);
    wheelCommitTimer_.setInterval(kWheelCommitMs);
    connect(&wheelCommitTimer_, &QTimer::timeout, this, [this]() {
        if (wheelBinding_) commitKnob(*wheelBinding_);
        wheelBinding_ = nullptr;
    });
}

void ModulePanelItem::addKnob(const Modular::ParamDef& def, float value, QPointF pos)
{
    auto* knob = new RotaryKnob();
    knob->setAccessibleName(QString("%1 %2").arg(moduleId_, displayName(def.name)));
    knob->setRange(0.0, 1.0);
    knob->setValue(toKnob(def, value));
    knob->setLabel(displayName(def.name));
    knob->setToolTip(tooltipFor(def, value));
    knob->setFixedSize(int(KNOB_WIDTH), int(KNOB_HEIGHT));
    knob->setAttribute(Qt::WA_TranslucentBackground);
    knob->installEventFilter(this);

    auto* proxy = new QGraphicsProxyWidget(this);
    proxy->setWidget(knob);
    proxy->setPos(pos);

    knobs_.push_back({ knob, QString::fromStdString(def.name), def, value });

    // Preview only: the label shows the value while dragging; the patch is
    // touched once, when the gesture ends.
    const int index = int(knobs_.size()) - 1;
    connect(knob, &RotaryKnob::valueChanged, this, [this, index](double n) {
        auto& b = knobs_[size_t(index)];
        const float v = fromKnob(b.def, n);
        b.knob->setLabel(formatValue(b.def, v));
        b.knob->setToolTip(tooltipFor(b.def, v));
    });
}

void ModulePanelItem::addChoice(const Modular::ParamDef& def, int index, qreal y)
{
    auto* combo = new QComboBox();
    combo->setAccessibleName(QString("%1 %2").arg(moduleId_, displayName(def.name)));
    for (const auto& c : def.choices)
        combo->addItem(QString::fromStdString(c));
    combo->setCurrentIndex(std::clamp(index, 0, int(def.choices.size()) - 1));
    combo->setToolTip(QString::fromStdString(def.description));
    combo->setFixedSize(int(WIDTH - kChoiceLabelWidth - 12.0), int(CHOICE_ROW_HEIGHT - 4.0));
    combo->setStyleSheet("QComboBox { font-size: 9px; padding: 0px 4px; }");

    auto* proxy = new QGraphicsProxyWidget(this);
    proxy->setWidget(combo);
    proxy->setPos(kChoiceLabelWidth + 4.0, y + 2.0);

    choiceLabels_ << displayName(def.name);
    choiceRowsY_.push_back(y);

    const QString param = QString::fromStdString(def.name);
    connect(combo, &QComboBox::activated, this, [this, param](int i) {
        emit paramCommitted(param, float(i));
    });
}

ModuleJack* ModulePanelItem::findJack(const QString& port, bool input) const
{
    for (auto* j : input ? inputJacks_ : outputJacks_)
        if (j->port().port == port) return j;
    return nullptr;
}

RotaryKnob* ModulePanelItem::knob(const QString& param) const
{
    for (const auto& b : knobs_)
        if (b.param == param) return b.knob;
    return nullptr;
}

ModulePanelItem::KnobBinding* ModulePanelItem::bindingFor(QObject* knob)
{
    for (auto& b : knobs_)
        if (b.knob == knob) return &b;
    return nullptr;
}

void ModulePanelItem::commitKnob(KnobBinding& b)
{
    if (!b.knob) return;
    b.knob->setLabel(displayName(b.def.name));
    const float v = fromKnob(b.def, b.knob->value());
    if (std::abs(v - b.committed) <= 1e-6f * std::max(1.0f, std::abs(v))) return;
    b.committed = v;
    emit paramCommitted(b.param, v);
}

bool ModulePanelItem::eventFilter(QObject* watched, QEvent* event)
{
    auto* b = bindingFor(watched);
    if (!b) return QGraphicsObject::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        // Releasing doesn't move the knob, so its value is already final.
        commitKnob(*b);
    }
    if (event->type() == QEvent::Wheel) {
        // Wheel ticks have no release: commit once the wheel goes quiet.
        if (wheelBinding_ && wheelBinding_ != b) commitKnob(*wheelBinding_);
        wheelBinding_ = b;
        wheelCommitTimer_.start();
    }
    return QGraphicsObject::eventFilter(watched, event);
}

QRectF ModulePanelItem::boundingRect() const
{
    const qreal jackPad = RoutingNode::JACK_OUTER_DIAMETER / 2.0 + 1.0;
    return QRectF(-jackPad, 0.0, WIDTH + 2 * jackPad, height_);
}

void ModulePanelItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    auto& theme = ThemeManager::instance().current();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRectF body(0.0, 0.0, WIDTH, height_);
    const QColor sectionColor = section_ == Modular::Section::Voice
        ? QColor(0, 150, 136) : QColor(126, 87, 194);

    painter->setPen(QPen(isSelected() ? theme.accentLight : theme.border, isSelected() ? 2.0 : 1.0));
    painter->setBrush(theme.surface);
    painter->drawRoundedRect(body, 6, 6);

    QPainterPath header;
    header.addRoundedRect(QRectF(0.0, 0.0, WIDTH, HEADER_HEIGHT), 6, 6);
    header.addRect(QRectF(0.0, HEADER_HEIGHT / 2, WIDTH, HEADER_HEIGHT / 2));
    painter->setPen(Qt::NoPen);
    painter->setBrush(sectionColor.darker(140));
    painter->drawPath(header.simplified());

    QFont titleFont = painter->font();
    titleFont.setPixelSize(11);
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(theme.text);
    painter->drawText(QRectF(8, 0, WIDTH - 16, HEADER_HEIGHT), Qt::AlignVCenter | Qt::AlignLeft,
                      moduleId_);
    QFont typeFont = titleFont;
    typeFont.setBold(false);
    typeFont.setPixelSize(9);
    painter->setFont(typeFont);
    painter->setPen(theme.text.darker(130));
    painter->drawText(QRectF(8, 0, WIDTH - 16, HEADER_HEIGHT), Qt::AlignVCenter | Qt::AlignRight,
                      type_);

    painter->setPen(theme.textDim);
    for (int i = 0; i < inputLabels_.size(); ++i)
        painter->drawText(QRectF(12, inputJacks_[size_t(i)]->y() - JACK_ROW_HEIGHT / 2,
                                 WIDTH / 2 - 12, JACK_ROW_HEIGHT),
                          Qt::AlignVCenter | Qt::AlignLeft, inputLabels_[i]);
    for (int i = 0; i < outputLabels_.size(); ++i)
        painter->drawText(QRectF(WIDTH / 2, outputJacks_[size_t(i)]->y() - JACK_ROW_HEIGHT / 2,
                                 WIDTH / 2 - 12, JACK_ROW_HEIGHT),
                          Qt::AlignVCenter | Qt::AlignRight, outputLabels_[i]);
    for (int i = 0; i < choiceLabels_.size(); ++i)
        painter->drawText(QRectF(8, choiceRowsY_[size_t(i)], kChoiceLabelWidth - 8, CHOICE_ROW_HEIGHT),
                          Qt::AlignVCenter | Qt::AlignLeft, choiceLabels_[i]);
}

QVariant ModulePanelItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged)
        emit moved();
    if (change == ItemSelectedHasChanged)
        update();
    return QGraphicsObject::itemChange(change, value);
}

void ModulePanelItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    pressPos_ = pos();
    setCursor(Qt::ClosedHandCursor);
    QGraphicsObject::mousePressEvent(event);
}

void ModulePanelItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    QGraphicsObject::mouseReleaseEvent(event);
    setCursor(Qt::OpenHandCursor);
    if (event->button() == Qt::LeftButton && pos() != pressPos_)
        emit dragFinished();
}

} // namespace OpenDaw
