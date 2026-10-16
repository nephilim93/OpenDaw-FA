#pragma once

#include "drums/DrumKit.h"
#include <QList>
#include <QUrl>
#include <QWidget>

namespace OpenDaw {

// One pad of the Drums tab's grid: name, note and source. It never edits the
// kit itself; it reports clicks, menus and dropped files to the view.
class DrumPadWidget : public QWidget {
    Q_OBJECT

public:
    explicit DrumPadWidget(const Drums::Pad& pad, QWidget* parent = nullptr);

    int note() const { return note_; }
    QString sourceText() const { return sourceText_; }
    void setSelected(bool selected);
    bool isSelected() const { return selected_; }
    // Why the pad is silent (shown in red); empty when it plays.
    void setWarning(const QString& warning);
    QString warning() const { return warning_; }

    QSize sizeHint() const override { return { 150, 64 }; }
    QSize minimumSizeHint() const override { return { 110, 58 }; }

signals:
    void pressed(int note);
    void menuRequested(int note, const QPoint& globalPos);
    void filesDropped(int note, const QList<QUrl>& urls);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    int note_;
    QString name_;
    QString sourceText_;
    bool isSample_ = false;
    bool selected_ = false;
    bool dropHover_ = false;
    QString warning_;
    QString baseToolTip_;
};

} // namespace OpenDaw
