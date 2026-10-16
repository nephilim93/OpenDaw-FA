#pragma once

#include "drums/DrumKit.h"
#include "modular/ModuleRegistry.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <vector>

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace OpenDaw {

class RotaryKnob;

// The Drums tab's editor for the selected pad: name, source, voice and its
// parameters or sample file, and level / pan / tune / choke. Like the
// Modular editor's panels it never edits the kit itself: each finished
// gesture becomes one change handed to the view (one undo step each).
class DrumPadEditor : public QWidget {
    Q_OBJECT

public:
    using Change = std::function<void(Drums::Pad&)>;
    // Applies `change` to the pad on `note` as one undo step named undoName.
    using EditHandler = std::function<void(int note, const Change& change, const QString& undoName)>;

    explicit DrumPadEditor(QWidget* parent = nullptr);

    void setEditHandler(EditHandler handler) { onEdit_ = std::move(handler); }
    // Shows `pad` (nullptr: nothing selected). Rebuilds the voice's knobs
    // only when the voice changes, so a commit doesn't flicker.
    void setPad(const Drums::Pad* pad);
    int note() const { return note_; }

    // True while a knob is held or a wheel turn awaits its commit: the view
    // waits before rebuilding.
    bool gestureActive() const { return gestureActive_ || wheelCommitTimer_.isActive(); }
    // "level", "pan" or "tune".
    RotaryKnob* mixKnob(const QString& name) const;
    // The shown voice's numeric and choice parameters.
    RotaryKnob* paramKnob(const QString& param) const;
    QComboBox* paramChoice(const QString& param) const;
    QComboBox* voiceBox() const { return voiceBox_; }
    QSpinBox* chokeBox() const { return chokeBox_; }

    static const Modular::ParamDef& levelDef();
    static const Modular::ParamDef& panDef();
    static const Modular::ParamDef& tuneDef();

signals:
    // The Sample switch or Browse… button: the view asks for a file.
    void browseRequested(int note);

private:
    struct KnobBinding {
        QPointer<RotaryKnob> knob;
        QString name;
        Modular::ParamDef def;
        float committed = 0.0f;
        std::function<void(Drums::Pad&, float)> apply;
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    RotaryKnob* addKnob(std::vector<KnobBinding>& bindings, QWidget* parent, const QString& name,
                        const Modular::ParamDef& def, float value,
                        std::function<void(Drums::Pad&, float)> apply);
    void setKnobValue(KnobBinding& b, float value);
    KnobBinding* bindingFor(QObject* knob);
    void commitKnob(KnobBinding& b);
    void rebuildVoiceParams(const Drums::Pad& pad);
    void updateVoiceParams(const Drums::Pad& pad);
    void edit(const Change& change, const QString& undoName);

    EditHandler onEdit_;
    int note_ = -1;
    QString shownModule_;

    QStackedWidget* stack_ = nullptr;
    QLabel* headingLabel_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QButtonGroup* sourceGroup_ = nullptr;
    QPushButton* synthButton_ = nullptr;
    QPushButton* sampleButton_ = nullptr;
    QStackedWidget* sourceStack_ = nullptr;
    QComboBox* voiceBox_ = nullptr;
    QWidget* voiceParams_ = nullptr;
    QGridLayout* voiceParamsLayout_ = nullptr;
    QLineEdit* sampleEdit_ = nullptr;
    QSpinBox* chokeBox_ = nullptr;

    // Bindings are referenced by pointer from the wheel timer: the mix
    // knobs live in their own vector, which never grows after construction.
    std::vector<KnobBinding> mixKnobs_;
    std::vector<KnobBinding> voiceKnobs_;
    std::vector<std::pair<QString, QPointer<QComboBox>>> voiceChoices_;
    KnobBinding* wheelBinding_ = nullptr;
    QTimer wheelCommitTimer_;
    bool gestureActive_ = false;
};

} // namespace OpenDaw
