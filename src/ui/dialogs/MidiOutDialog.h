#pragma once

#include <QDialog>

class QComboBox;
class QLabel;

namespace OpenDaw {

class MidiOutPlugin;

// Where a MIDI Out track sends: OpenDaw's virtual port or a system MIDI
// output, and on which channel. Each change applies at once as one undo step.
class MidiOutDialog : public QDialog {
    Q_OBJECT

public:
    MidiOutDialog(MidiOutPlugin& plugin, QWidget* parent = nullptr);

    static void showFor(MidiOutPlugin& plugin, QWidget* parent);

    QComboBox* destinationBox() const { return destination_; }
    QComboBox* channelBox() const { return channel_; }
    QString statusText() const;

private:
    void fillDestinations();
    void updateStatus();

    MidiOutPlugin& plugin_;
    QComboBox* destination_ = nullptr;
    QComboBox* channel_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace OpenDaw
