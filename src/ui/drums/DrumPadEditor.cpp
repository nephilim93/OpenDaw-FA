#include "DrumPadEditor.h"
#include "GmDrumNames.h"
#include "ui/controls/ParamKnobMapping.h"
#include "ui/controls/RotaryKnob.h"
#include "utils/ThemeManager.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace OpenDaw {

namespace {

constexpr int kKnobWidth = 56;
constexpr int kKnobHeight = 50;
constexpr int kKnobsPerRow = 6;
constexpr int kWheelCommitMs = 400;

Modular::ParamDef mixParam(const char* name, float min, float max, float def, const char* unit,
                           const char* description)
{
    Modular::ParamDef p;
    p.name = name;
    p.min = min;
    p.max = max;
    p.defaultValue = def;
    p.unit = unit;
    p.description = description;
    return p;
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold;")
                             .arg(ThemeManager::instance().current().textDim.name()));
    return label;
}

} // namespace

const Modular::ParamDef& DrumPadEditor::levelDef()
{
    static const auto def = mixParam("level", 0.0f, 1.5f, 0.8f, "", "Pad volume (linear gain, 1 = unity).");
    return def;
}

const Modular::ParamDef& DrumPadEditor::panDef()
{
    static const auto def = mixParam("pan", -1.0f, 1.0f, 0.0f, "", "Stereo position: -1 left, 1 right.");
    return def;
}

const Modular::ParamDef& DrumPadEditor::tuneDef()
{
    static const auto def = mixParam("tune", -24.0f, 24.0f, 0.0f, "st", "Pitch shift in semitones.");
    return def;
}

DrumPadEditor::DrumPadEditor(QWidget* parent) : QWidget(parent)
{
    setAccessibleName("Pad Editor");
    auto& theme = ThemeManager::instance().current();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    outer->addWidget(stack_);

    auto* nothing = new QLabel("Select a pad to edit it.", stack_);
    nothing->setAlignment(Qt::AlignCenter);
    nothing->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme.textDim.name()));
    stack_->addWidget(nothing);

    auto* page = new QWidget(stack_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(6);

    headingLabel_ = new QLabel(page);
    headingLabel_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(theme.text.name()));
    layout->addWidget(headingLabel_);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("Name", page));
    nameEdit_ = new QLineEdit(page);
    nameEdit_->setAccessibleName("Pad Name");
    nameRow->addWidget(nameEdit_, 1);
    layout->addLayout(nameRow);
    connect(nameEdit_, &QLineEdit::editingFinished, this, [this]() {
        const QString name = nameEdit_->text().trimmed();
        if (name.isEmpty() || note_ < 0) return;
        edit([name](Drums::Pad& p) { p.name = name; }, "Rename Pad");
    });

    // ── Source: Synth voice or Sample file ──
    auto* sourceRow = new QHBoxLayout();
    sourceRow->addWidget(new QLabel("Source", page));
    synthButton_ = new QPushButton("Synth", page);
    synthButton_->setAccessibleName("Synth Source");
    synthButton_->setToolTip("Play one of the built-in drum voices");
    sampleButton_ = new QPushButton("Sample", page);
    sampleButton_->setAccessibleName("Sample Source");
    sampleButton_->setToolTip("Play an audio file");
    sourceGroup_ = new QButtonGroup(this);
    for (auto* b : { synthButton_, sampleButton_ }) {
        b->setCheckable(true);
        sourceGroup_->addButton(b);
        sourceRow->addWidget(b);
    }
    sourceRow->addStretch();
    layout->addLayout(sourceRow);
    connect(synthButton_, &QPushButton::clicked, this, [this]() {
        if (note_ < 0) return;
        const auto voice = Drums::defaultPadForNote(note_);
        edit([voice](Drums::Pad& p) {
            if (p.source == Drums::PadSource::Synth) return;
            p.source = Drums::PadSource::Synth;
            p.module = voice.module;
            p.params = voice.params;
            p.sample.clear();
        }, "Change Source");
    });
    // A sample pad needs a file first: the Browse… flow switches the source.
    connect(sampleButton_, &QPushButton::clicked, this, [this]() {
        if (note_ < 0) return;
        if (sourceStack_->currentIndex() == 0) synthButton_->setChecked(true);
        emit browseRequested(note_);
    });

    sourceStack_ = new QStackedWidget(page);
    sourceStack_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto* synthPage = new QWidget(sourceStack_);
    auto* synthLayout = new QVBoxLayout(synthPage);
    synthLayout->setContentsMargins(0, 0, 0, 0);
    auto* voiceRow = new QHBoxLayout();
    voiceRow->addWidget(new QLabel("Voice", synthPage));
    voiceBox_ = new QComboBox(synthPage);
    voiceBox_->setAccessibleName("Voice");
    for (const auto& type : Drums::synthPadModules()) {
        voiceBox_->addItem(ParamKnob::displayName(type.toStdString()), type);
        if (const auto* def = Modular::findModule(type.toStdString()))
            voiceBox_->setItemData(voiceBox_->count() - 1, QString::fromStdString(def->description),
                                   Qt::ToolTipRole);
    }
    voiceRow->addWidget(voiceBox_, 1);
    synthLayout->addLayout(voiceRow);
    connect(voiceBox_, &QComboBox::activated, this, [this](int index) {
        const QString type = voiceBox_->itemData(index).toString();
        edit([type](Drums::Pad& p) {
            if (p.module == type) return;
            p.module = type;
            p.params.clear();   // they belonged to the old voice
        }, "Change Voice");
    });
    voiceParams_ = new QWidget(synthPage);
    voiceParamsLayout_ = new QGridLayout(voiceParams_);
    voiceParamsLayout_->setContentsMargins(0, 0, 0, 0);
    voiceParamsLayout_->setHorizontalSpacing(2);
    voiceParamsLayout_->setVerticalSpacing(2);
    synthLayout->addWidget(voiceParams_);
    sourceStack_->addWidget(synthPage);

    auto* samplePage = new QWidget(sourceStack_);
    auto* sampleLayout = new QHBoxLayout(samplePage);
    sampleLayout->setContentsMargins(0, 0, 0, 0);
    sampleLayout->addWidget(new QLabel("File", samplePage));
    sampleEdit_ = new QLineEdit(samplePage);
    sampleEdit_->setAccessibleName("Sample File");
    sampleEdit_->setReadOnly(true);
    sampleLayout->addWidget(sampleEdit_, 1);
    auto* browse = new QPushButton(QString::fromUtf8("Browse…"), samplePage);
    browse->setAccessibleName("Browse Sample");
    connect(browse, &QPushButton::clicked, this, [this]() {
        if (note_ >= 0) emit browseRequested(note_);
    });
    sampleLayout->addWidget(browse);
    sourceStack_->addWidget(samplePage);
    layout->addWidget(sourceStack_);

    // ── Mix: level, pan, tune, choke ──
    layout->addWidget(sectionLabel("MIX", page));
    auto* mix = new QWidget(page);
    auto* mixLayout = new QHBoxLayout(mix);
    mixLayout->setContentsMargins(0, 0, 0, 0);
    mixLayout->setSpacing(2);
    mixKnobs_.reserve(3);
    mixLayout->addWidget(addKnob(mixKnobs_, mix, "level", levelDef(), levelDef().defaultValue,
                                 [](Drums::Pad& p, float v) { p.level = v; }));
    mixLayout->addWidget(addKnob(mixKnobs_, mix, "pan", panDef(), panDef().defaultValue,
                                 [](Drums::Pad& p, float v) { p.pan = v; }));
    mixLayout->addWidget(addKnob(mixKnobs_, mix, "tune", tuneDef(), tuneDef().defaultValue,
                                 [](Drums::Pad& p, float v) { p.tune = v; }));
    auto* chokeColumn = new QVBoxLayout();
    chokeColumn->addWidget(new QLabel("Choke", mix), 0, Qt::AlignHCenter);
    chokeBox_ = new QSpinBox(mix);
    chokeBox_->setAccessibleName("Choke Group");
    chokeBox_->setToolTip("Pads in the same group cut each other off (e.g. closed and open hats)");
    chokeBox_->setRange(0, 16);
    chokeBox_->setSpecialValueText("None");
    // Typing "12" must not commit a "1" on the way.
    chokeBox_->setKeyboardTracking(false);
    chokeColumn->addWidget(chokeBox_);
    mixLayout->addLayout(chokeColumn);
    mixLayout->addStretch();
    layout->addWidget(mix);
    connect(chokeBox_, &QSpinBox::valueChanged, this, [this](int group) {
        edit([group](Drums::Pad& p) { p.choke = group; }, "Change Pad");
    });

    layout->addStretch();
    stack_->addWidget(page);

    wheelCommitTimer_.setSingleShot(true);
    wheelCommitTimer_.setInterval(kWheelCommitMs);
    connect(&wheelCommitTimer_, &QTimer::timeout, this, [this]() {
        if (wheelBinding_) commitKnob(*wheelBinding_);
        wheelBinding_ = nullptr;
    });
}

RotaryKnob* DrumPadEditor::addKnob(std::vector<KnobBinding>& bindings, QWidget* parent,
                                   const QString& name, const Modular::ParamDef& def,
                                   float value, std::function<void(Drums::Pad&, float)> apply)
{
    auto* knob = new RotaryKnob(parent);
    knob->setAccessibleName((&bindings == &mixKnobs_ ? "Pad " : "Voice ")
                            + ParamKnob::displayName(def.name));
    knob->setRange(0.0, 1.0);
    knob->setFixedSize(kKnobWidth, kKnobHeight);
    knob->installEventFilter(this);

    bindings.push_back({ knob, name, def, value, std::move(apply) });
    setKnobValue(bindings.back(), value);

    // Preview only: the label shows the value while dragging; the kit is
    // touched once, when the gesture ends.
    connect(knob, &RotaryKnob::valueChanged, this, [this, knob](double) {
        if (auto* b = bindingFor(knob)) {
            const float v = ParamKnob::fromKnob(b->def, knob->value());
            knob->setLabel(ParamKnob::formatValue(b->def, v));
            knob->setToolTip(ParamKnob::tooltip(b->def, v));
        }
    });
    return knob;
}

void DrumPadEditor::setKnobValue(KnobBinding& b, float value)
{
    b.committed = value;
    if (!b.knob) return;
    const QSignalBlocker block(b.knob);
    b.knob->setValue(ParamKnob::toKnob(b.def, value));
    b.knob->setLabel(ParamKnob::displayName(b.def.name));
    b.knob->setToolTip(ParamKnob::tooltip(b.def, value));
}

DrumPadEditor::KnobBinding* DrumPadEditor::bindingFor(QObject* knob)
{
    for (auto* list : { &mixKnobs_, &voiceKnobs_ })
        for (auto& b : *list)
            if (b.knob == knob) return &b;
    return nullptr;
}

RotaryKnob* DrumPadEditor::mixKnob(const QString& name) const
{
    for (const auto& b : mixKnobs_)
        if (b.name == name) return b.knob;
    return nullptr;
}

RotaryKnob* DrumPadEditor::paramKnob(const QString& param) const
{
    for (const auto& b : voiceKnobs_)
        if (b.name == param) return b.knob;
    return nullptr;
}

QComboBox* DrumPadEditor::paramChoice(const QString& param) const
{
    for (const auto& [name, box] : voiceChoices_)
        if (name == param) return box;
    return nullptr;
}

void DrumPadEditor::edit(const Change& change, const QString& undoName)
{
    if (onEdit_ && note_ >= 0) onEdit_(note_, change, undoName);
}

void DrumPadEditor::commitKnob(KnobBinding& b)
{
    if (!b.knob) return;
    b.knob->setLabel(ParamKnob::displayName(b.def.name));
    const float v = ParamKnob::fromKnob(b.def, b.knob->value());
    if (std::abs(v - b.committed) <= 1e-6f * std::max(1.0f, std::abs(v))) return;
    b.committed = v;
    auto apply = b.apply;
    edit([apply, v](Drums::Pad& p) { apply(p, v); }, "Change Pad");
}

bool DrumPadEditor::eventFilter(QObject* watched, QEvent* event)
{
    auto* b = bindingFor(watched);
    if (!b) return QWidget::eventFilter(watched, event);

    const bool left = (event->type() == QEvent::MouseButtonPress
                       || event->type() == QEvent::MouseButtonRelease)
                      && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
    if (left && event->type() == QEvent::MouseButtonPress)
        gestureActive_ = true;
    if (left && event->type() == QEvent::MouseButtonRelease) {
        gestureActive_ = false;
        // Releasing doesn't move the knob, so its value is already final.
        commitKnob(*b);
    }
    if (event->type() == QEvent::Wheel) {
        // Wheel ticks have no release: commit once the wheel goes quiet.
        if (wheelBinding_ && wheelBinding_ != b) commitKnob(*wheelBinding_);
        wheelBinding_ = b;
        wheelCommitTimer_.start();
    }
    return QWidget::eventFilter(watched, event);
}

void DrumPadEditor::setPad(const Drums::Pad* pad)
{
    // A pending wheel gesture belongs to the pad it was made on.
    if (wheelCommitTimer_.isActive()) {
        wheelCommitTimer_.stop();
        if (wheelBinding_) commitKnob(*wheelBinding_);
    }
    wheelBinding_ = nullptr;
    gestureActive_ = false;

    if (!pad) {
        note_ = -1;
        stack_->setCurrentIndex(0);
        return;
    }
    note_ = pad->note;
    stack_->setCurrentIndex(1);
    headingLabel_->setText(QString("Pad %1 · %2").arg(pad->note).arg(Drums::gmDrumName(pad->note)));
    {
        const QSignalBlocker block(nameEdit_);
        if (!nameEdit_->hasFocus() || nameEdit_->text().trimmed() == pad->name)
            nameEdit_->setText(pad->name);
    }
    const bool isSample = pad->source == Drums::PadSource::Sample;
    (isSample ? sampleButton_ : synthButton_)->setChecked(true);
    sourceStack_->setCurrentIndex(isSample ? 1 : 0);
    sampleEdit_->setText(isSample ? pad->sample : QString());
    sampleEdit_->setToolTip(sampleEdit_->text());
    {
        const QSignalBlocker block(voiceBox_);
        voiceBox_->setCurrentIndex(isSample ? -1 : voiceBox_->findData(pad->module));
    }
    const QString module = isSample ? QString() : pad->module;
    if (module != shownModule_) rebuildVoiceParams(*pad);
    else updateVoiceParams(*pad);

    setKnobValue(mixKnobs_[0], pad->level);
    setKnobValue(mixKnobs_[1], pad->pan);
    setKnobValue(mixKnobs_[2], pad->tune);
    const QSignalBlocker block(chokeBox_);
    chokeBox_->setValue(pad->choke);
}

void DrumPadEditor::rebuildVoiceParams(const Drums::Pad& pad)
{
    while (auto* item = voiceParamsLayout_->takeAt(0)) {
        if (auto* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    voiceKnobs_.clear();
    voiceChoices_.clear();
    shownModule_ = pad.source == Drums::PadSource::Synth ? pad.module : QString();

    const auto* def = shownModule_.isEmpty() ? nullptr : Modular::findModule(shownModule_.toStdString());
    if (!def) return;
    voiceKnobs_.reserve(def->params.size());   // the wheel timer points into it
    int knobIndex = 0, row = 0;
    for (const auto& pd : def->params) {
        if (!pd.choices.empty()) continue;
        const QString name = QString::fromStdString(pd.name);
        auto* k = addKnob(voiceKnobs_, voiceParams_, name, pd, pad.params.value(name, pd.defaultValue),
                          [name](Drums::Pad& p, float v) { p.params[name] = v; });
        voiceParamsLayout_->addWidget(k, knobIndex / kKnobsPerRow, knobIndex % kKnobsPerRow);
        ++knobIndex;
    }
    row = (knobIndex + kKnobsPerRow - 1) / kKnobsPerRow;
    for (const auto& pd : def->params) {
        if (pd.choices.empty()) continue;
        const QString name = QString::fromStdString(pd.name);
        auto* label = new QLabel(ParamKnob::displayName(pd.name), voiceParams_);
        auto* box = new QComboBox(voiceParams_);
        box->setAccessibleName(ParamKnob::displayName(pd.name));
        box->setToolTip(QString::fromStdString(pd.description));
        for (const auto& c : pd.choices) box->addItem(QString::fromStdString(c));
        box->setCurrentIndex(std::clamp(int(std::lround(pad.params.value(name, pd.defaultValue))),
                                        0, int(pd.choices.size()) - 1));
        connect(box, &QComboBox::activated, this, [this, name](int i) {
            edit([name, i](Drums::Pad& p) { p.params[name] = float(i); }, "Change Pad");
        });
        voiceParamsLayout_->addWidget(label, row, 0, 1, 2);
        voiceParamsLayout_->addWidget(box, row, 2, 1, kKnobsPerRow - 2);
        voiceChoices_.emplace_back(name, box);
        ++row;
    }
    voiceParamsLayout_->setColumnStretch(kKnobsPerRow, 1);
}

void DrumPadEditor::updateVoiceParams(const Drums::Pad& pad)
{
    const auto* def = shownModule_.isEmpty() ? nullptr : Modular::findModule(shownModule_.toStdString());
    if (!def) return;
    for (auto& b : voiceKnobs_)
        setKnobValue(b, pad.params.value(b.name, b.def.defaultValue));
    for (auto& [name, box] : voiceChoices_) {
        const int pi = def->paramIndex(name.toStdString());
        if (!box || pi < 0) continue;
        const auto& pd = def->params[size_t(pi)];
        const QSignalBlocker block(box);
        box->setCurrentIndex(std::clamp(int(std::lround(pad.params.value(name, pd.defaultValue))),
                                        0, int(pd.choices.size()) - 1));
    }
}

} // namespace OpenDaw
