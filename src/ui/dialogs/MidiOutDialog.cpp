#include "MidiOutDialog.h"
#include "engine/midiports/MidiOutPlugin.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace OpenDaw {

namespace {
QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }
juce::String toJuce(const QString& s) { return juce::String::fromUTF8(s.toUtf8().constData()); }
} // namespace

MidiOutDialog::MidiOutDialog(MidiOutPlugin& plugin, QWidget* parent)
    : QDialog(parent), plugin_(plugin)
{
    setWindowTitle("MIDI Out");
    setAccessibleName("MIDI Out Settings");
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("This track plays another app or device: its clips and live input "
                             "are sent as MIDI, and the track itself makes no sound.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    destination_ = new QComboBox(this);
    destination_->setAccessibleName("MIDI Out destination");
    form->addRow("Send to:", destination_);
    channel_ = new QComboBox(this);
    channel_->setAccessibleName("MIDI Out channel");
    channel_->addItem("Keep each note's channel", 0);
    for (int c = 1; c <= 16; ++c) channel_->addItem(QString("Channel %1").arg(c), c);
    channel_->setCurrentIndex(std::max(0, channel_->findData(plugin_.channel())));
    form->addRow("Channel:", channel_);
    layout->addLayout(form);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* hint = new QLabel(this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray; font-size: 11px;");
    hint->setText(VirtualMidiPorts::supported()
        ? "In the other app (VCV Rack, midimux, ...) choose the MIDI input \"OpenDaw Out\". "
          "To play OpenDaw from it, send to \"OpenDaw In\" and pick that as a track's input."
        : "Windows has no virtual MIDI ports for apps: install loopMIDI (free), create a port, "
          "choose it here, and choose the same port in the other app.");
    layout->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* refresh = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);
    refresh->setToolTip("Look for MIDI outputs connected since this window opened");
    connect(refresh, &QPushButton::clicked, this, [this]() { fillDestinations(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    fillDestinations();

    connect(destination_, &QComboBox::activated, this, [this]() {
        const QString dest = destination_->currentData().toString();
        if (toJuce(dest) != plugin_.destination())
            plugin_.setDestination(toJuce(dest), "Change MIDI Out Destination");
        updateStatus();
    });
    connect(channel_, &QComboBox::activated, this, [this]() {
        const int ch = channel_->currentData().toInt();
        if (ch != plugin_.channel())
            plugin_.setChannel(ch, "Change MIDI Out Channel");
    });
}

void MidiOutDialog::showFor(MidiOutPlugin& plugin, QWidget* parent)
{
    MidiOutDialog dialog(plugin, parent);
    dialog.exec();
}

void MidiOutDialog::fillDestinations()
{
    auto* ports = VirtualMidiPorts::instance();
    const QStringList list = ports ? ports->destinations() : QStringList();
    const QString current = toQ(plugin_.destination());

    destination_->clear();
    for (const auto& d : list)
        destination_->addItem(d == VirtualMidiPorts::kVirtualDestination ? d + " (other apps)" : d, d);
    // A saved destination that isn't here now (unplugged) stays selectable.
    if (!current.isEmpty() && !list.contains(current))
        destination_->addItem(current + " (not connected)", current);
    if (destination_->count() == 0) destination_->addItem("No MIDI outputs found", QString());
    destination_->setCurrentIndex(std::max(0, destination_->findData(current)));
    updateStatus();
}

void MidiOutDialog::updateStatus()
{
    const QString problem = toQ(plugin_.problem());
    status_->setText(problem.isEmpty() ? QString("Sending to %1.").arg(toQ(plugin_.destination())) : problem);
    status_->setStyleSheet(problem.isEmpty() ? QString() : QString("color: #d9822b;"));
}

QString MidiOutDialog::statusText() const
{
    return status_->text();
}

} // namespace OpenDaw
