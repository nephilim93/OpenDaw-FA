#pragma once

#include <QWidget>
#include <tracktion_engine/tracktion_engine.h>
#include <vector>
#include <set>

namespace te = tracktion::engine;

namespace OpenDaw {

class VelocityLane : public QWidget {
    Q_OBJECT

public:
    explicit VelocityLane(QWidget* parent = nullptr);

    void setClip(te::MidiClip* clip);
    void setClips(const std::vector<te::MidiClip*>& clips, const std::set<int>& hidden);
    void setPixelsPerBeat(double ppb) { pixelsPerBeat_ = ppb; update(); }
    void setScrollOffset(int x) { scrollOffset_ = x; update(); }
    void setSelectedNotes(const std::set<te::MidiNote*>& notes) { selectedNotes_ = notes; update(); }
    void refresh();

signals:
    void velocityChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct Stem { te::MidiNote* note; QColor color; bool selected; };
    std::vector<Stem> visibleStems() const;
    std::vector<te::MidiNote*> notesToEditAt(double x) const;
    double stemX(const te::MidiNote* note) const;
    int velocityAtY(double y) const;
    void applyVelocityAt(const QPointF& pos);

    std::vector<te::MidiClip*> allClips_;
    std::set<int> hiddenChannels_;
    te::MidiClip* primaryClip_ = nullptr;
    double pixelsPerBeat_ = 60.0;
    int scrollOffset_ = 0;
    std::set<te::MidiNote*> selectedNotes_;
};

} // namespace OpenDaw
