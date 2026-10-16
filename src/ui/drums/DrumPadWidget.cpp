#include "DrumPadWidget.h"
#include "GmDrumNames.h"
#include "ui/controls/ParamKnobMapping.h"
#include "utils/ThemeManager.h"
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

namespace OpenDaw {

DrumPadWidget::DrumPadWidget(const Drums::Pad& pad, QWidget* parent)
    : QWidget(parent), note_(pad.note), name_(pad.name), isSample_(pad.source == Drums::PadSource::Sample)
{
    sourceText_ = isSample_ ? QFileInfo(pad.sample).fileName()
                            : ParamKnob::displayName(pad.module.toStdString());
    const QString label = name_.isEmpty() ? Drums::gmDrumName(note_) : name_;
    setAccessibleName(QString("Pad %1 %2").arg(note_).arg(label));
    baseToolTip_ = QString("%1\nNote %2 (%3)\n%4: %5\nClick to play, right-click for more, "
                           "drop an audio file to use it")
                       .arg(label).arg(note_).arg(Drums::gmDrumName(note_),
                            isSample_ ? "Sample" : "Voice", isSample_ ? pad.sample : sourceText_);
    setToolTip(baseToolTip_);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void DrumPadWidget::setSelected(bool selected)
{
    if (selected_ == selected) return;
    selected_ = selected;
    update();
}

void DrumPadWidget::setWarning(const QString& warning)
{
    warning_ = warning;
    setToolTip(warning.isEmpty() ? baseToolTip_ : warning + "\n\n" + baseToolTip_);
    update();
}

void DrumPadWidget::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    const QColor tint = isSample_ ? QColor(240, 160, 60) : theme.accent;
    QColor border = selected_ ? theme.accentLight : theme.border;
    if (!warning_.isEmpty()) border = theme.meterRed;
    if (dropHover_) border = QColor(240, 160, 60);
    p.setPen(QPen(border, selected_ || dropHover_ || !warning_.isEmpty() ? 2.0 : 1.0));
    p.setBrush(selected_ ? theme.surfaceLight : theme.surface);
    p.drawRoundedRect(r, 5, 5);
    // A strip down the left edge says synth or sample at a glance.
    p.setPen(Qt::NoPen);
    p.setBrush(tint.darker(selected_ ? 100 : 130));
    p.drawRoundedRect(QRectF(r.left() + 3, r.top() + 4, 3, r.height() - 8), 1.5, 1.5);

    const QRectF text = r.adjusted(12, 4, -6, -4);
    QFont f = font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.setPen(theme.text);
    const QString title = (warning_.isEmpty() ? QString() : QString::fromUtf8("⚠ "))
                          + (name_.isEmpty() ? Drums::gmDrumName(note_) : name_);
    p.drawText(text, Qt::AlignLeft | Qt::AlignTop,
               p.fontMetrics().elidedText(title, Qt::ElideRight, int(text.width())));

    f.setBold(false);
    f.setPixelSize(9);
    p.setFont(f);
    p.setPen(theme.textDim);
    const QString noteLine = QString("%1 · %2").arg(note_).arg(Drums::gmDrumName(note_));
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(noteLine, Qt::ElideRight, int(text.width())));
    p.setPen(warning_.isEmpty() ? tint.lighter(110) : theme.meterRed);
    p.drawText(text, Qt::AlignLeft | Qt::AlignBottom,
               p.fontMetrics().elidedText(sourceText_, Qt::ElideMiddle, int(text.width())));
}

void DrumPadWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit pressed(note_);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DrumPadWidget::contextMenuEvent(QContextMenuEvent* event)
{
    emit menuRequested(note_, event->globalPos());
    event->accept();
}

void DrumPadWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;
    event->acceptProposedAction();
    dropHover_ = true;
    update();
}

void DrumPadWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    dropHover_ = false;
    update();
}

void DrumPadWidget::dropEvent(QDropEvent* event)
{
    dropHover_ = false;
    update();
    if (!event->mimeData()->hasUrls()) return;
    event->acceptProposedAction();
    emit filesDropped(note_, event->mimeData()->urls());
}

} // namespace OpenDaw
