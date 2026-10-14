#include "VelocityLane.h"
#include "ChannelColors.h"
#include "utils/ThemeManager.h"
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace OpenDaw {

VelocityLane::VelocityLane(QWidget* parent)
    : QWidget(parent)
{
    setAccessibleName("Velocity Lane");
    setFixedHeight(60);
    setMouseTracking(true);
}

void VelocityLane::setClip(te::MidiClip* clip)
{
    primaryClip_ = clip;
    allClips_.clear();
    hiddenChannels_.clear();
    if (clip)
        allClips_.push_back(clip);
    update();
}

void VelocityLane::setClips(const std::vector<te::MidiClip*>& clips,
                             const std::set<int>& hidden)
{
    allClips_ = clips;
    hiddenChannels_ = hidden;
    primaryClip_ = clips.empty() ? nullptr : clips.front();
    update();
}

void VelocityLane::refresh()
{
    update();
}

namespace {
constexpr double kStemWidth = 3.0;
constexpr double kHitRadius = 5.0;  // px either side of a stem
constexpr int kMargin = 2;           // px above and below the bars
}

double VelocityLane::stemX(const te::MidiNote* note) const
{
    return note->getStartBeat().inBeats() * pixelsPerBeat_ - scrollOffset_;
}

int VelocityLane::velocityAtY(double y) const
{
    const double usable = std::max(1, height() - 2 * kMargin);
    const double v = 127.0 * (height() - kMargin - y) / usable;
    return std::clamp(static_cast<int>(std::lround(v)), 1, 127);
}

// Stems in draw order (later ones are painted on top): unselected notes by
// pitch so the highest note of a chord is on top, then selected notes.
std::vector<VelocityLane::Stem> VelocityLane::visibleStems() const
{
    std::vector<Stem> stems;
    for (auto* mc : allClips_) {
        int ch = mc->getMidiChannel().getChannelNumber();
        if (ch < 1) ch = 1;
        if (hiddenChannels_.count(ch)) continue;

        const QColor color = channelColor(ch);
        for (auto* note : mc->getSequence().getNotes()) {
            const double x = stemX(note);
            if (x + kStemWidth < -kHitRadius || x > width() + kHitRadius) continue;
            stems.push_back({ note, color, selectedNotes_.count(note) > 0 });
        }
    }
    std::stable_sort(stems.begin(), stems.end(), [](const Stem& a, const Stem& b) {
        if (a.selected != b.selected) return !a.selected;
        return a.note->getNoteNumber() < b.note->getNoteNumber();
    });
    return stems;
}

// The notes a click at x edits: the selected notes among the nearest stems
// (so selecting a note in a chord picks which one is edited), otherwise the
// single stem painted on top there.
std::vector<te::MidiNote*> VelocityLane::notesToEditAt(double x) const
{
    const auto stems = visibleStems();

    double nearest = kHitRadius + 1.0;
    for (const auto& st : stems)
        nearest = std::min(nearest, std::abs(stemX(st.note) + kStemWidth / 2 - x));
    if (nearest > kHitRadius) return {};

    std::vector<te::MidiNote*> selected;
    const Stem* topmost = nullptr;
    for (const auto& st : stems) {
        if (std::abs(stemX(st.note) + kStemWidth / 2 - x) > nearest + 0.5) continue;
        if (st.selected) selected.push_back(st.note);
        topmost = &st;
    }
    if (!selected.empty()) return selected;
    return { topmost->note };
}

void VelocityLane::paintEvent(QPaintEvent*)
{
    auto& theme = ThemeManager::instance().current();
    QPainter painter(this);

    painter.fillRect(rect(), theme.pianoRollBackground);

    if (!allClips_.empty()) {
        const double usable = std::max(1, height() - 2 * kMargin);
        for (const auto& st : visibleStems()) {
            const double barH = std::max(2.0, (st.note->getVelocity() / 127.0) * usable);
            const QRectF bar(stemX(st.note), height() - kMargin - barH, kStemWidth, barH);
            const QColor color = st.selected ? theme.pianoRollNoteSelected : st.color;
            painter.fillRect(bar, color);
            painter.setPen(QPen(color.darker(150), 0.5));
            painter.drawRect(bar);
        }
    }

    painter.setPen(QPen(theme.pianoRollGrid, 0.5));
    painter.drawLine(0, 0, width(), 0);
}

void VelocityLane::applyVelocityAt(const QPointF& pos)
{
    const auto notes = notesToEditAt(pos.x());
    if (notes.empty()) return;

    const int vel = velocityAtY(pos.y());
    auto* um = primaryClip_ ? &primaryClip_->edit.getUndoManager() : nullptr;
    for (auto* note : notes)
        note->setVelocity(vel, um);
    update();
    emit velocityChanged();
}

void VelocityLane::mousePressEvent(QMouseEvent* event)
{
    if (height() <= 0 || event->button() != Qt::LeftButton) return;
    if (primaryClip_)
        primaryClip_->edit.getUndoManager().beginNewTransaction("Change Velocity");
    applyVelocityAt(event->position());
}

// Dragging paints velocities across the stems the mouse passes over.
void VelocityLane::mouseMoveEvent(QMouseEvent* event)
{
    if (height() <= 0 || !(event->buttons() & Qt::LeftButton)) return;
    applyVelocityAt(event->position());
}

} // namespace OpenDaw
