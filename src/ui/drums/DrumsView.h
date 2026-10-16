#pragma once

#include "DrumPadEditor.h"
#include "drums/DrumKit.h"
#include "engine/EditManager.h"
#include <QList>
#include <QMap>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <vector>

class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace OpenDaw {

class DrumMachinePlugin;
class DrumPadWidget;
class SampleBrowser;

// The "Drums" tab: the selected track's Drum Machine kit as a grid of pads
// and an editor for the selected one. Every edit goes through
// DrumMachinePlugin::setKit as one named undo step; the view is rebuilt
// from the plugin state whenever the kit changes (editor, AI, undo/redo).
class DrumsView : public QWidget, private juce::ValueTree::Listener {
    Q_OBJECT

public:
    explicit DrumsView(EditManager* editMgr, QWidget* parent = nullptr);
    ~DrumsView() override;

    void setTrack(te::AudioTrack* track);
    te::AudioTrack* track() const { return track_; }
    DrumMachinePlugin* drumMachine() const;

    // The edit entry points the mouse, menus and drops use. They return
    // false (and flash the error) when the change is refused.
    bool applyKit(const Drums::Kit& kit, const QString& undoName);
    bool editPad(int note, const DrumPadEditor::Change& change, const QString& undoName);
    bool addPad();
    bool renamePad(int note, const QString& name);
    bool duplicatePad(int note, int toNote);
    bool removePad(int note);
    bool resetPad(int note);
    // A sample file onto the pad on `note`, or onto a new pad on the next
    // free note when `note` is -1.
    bool loadSample(int note, const QString& path);
    // What a drop does: exactly one audio file, else a short message.
    bool dropFiles(int note, const QList<QUrl>& urls);
    // Kit presets (Drums::listKitPresets): loading replaces the whole kit
    // as one undo step.
    bool loadKitPreset(const QString& name);
    bool saveKitPreset(const QString& name, bool overwrite);
    // Every sample in `folder` whose name says which drum it is, on its GM
    // note (Drums::buildKitFromFolder), as one undo step.
    bool buildKitFromFolder(const QString& folder);

    // Selects the pad (its editor shows) and plays it.
    void selectPad(int note, bool audition = true);
    void auditionPad(int note);

    // Introspection for tests and accessibility.
    int padCount() const { return int(pads_.size()); }
    DrumPadWidget* padWidget(int note) const;
    DrumPadEditor* padEditor() const { return editor_; }
    SampleBrowser* sampleBrowser() const { return sampleBrowser_; }
    int selectedNote() const { return selectedNote_; }
    QString lastError() const { return lastError_; }
    QString warningText() const;
    int rebuildCount() const { return rebuildCount_; }

    static bool isAudioFile(const QString& path);

public slots:
    void rebuild();
    void scheduleRebuild();

private:
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& id) override;

    void refreshTarget();
    void attach(DrumMachinePlugin* plugin);
    bool trackStillExists() const;
    void updatePage();
    void updateSelection();
    void showError(const QString& text);
    void showPadMenu(int note, const QPoint& globalPos);
    void showKitsMenu(const QPoint& globalPos);
    void promptSaveKitPreset();
    void browseSample(int note);
    int nextFreeNote() const;

    EditManager* editMgr_;
    te::AudioTrack* track_ = nullptr;
    te::Plugin::Ptr plugin_;
    juce::ValueTree pluginState_;
    Drums::Kit kit_;   // what the view shows

    QStackedWidget* stack_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* warningBar_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QWidget* gridArea_ = nullptr;
    QGridLayout* gridLayout_ = nullptr;
    DrumPadEditor* editor_ = nullptr;
    SampleBrowser* sampleBrowser_ = nullptr;

    std::vector<DrumPadWidget*> pads_;
    int selectedNote_ = -1;
    int rebuildCount_ = 0;
    QString lastError_;
    QTimer rebuildTimer_;
    QTimer errorTimer_;
};

} // namespace OpenDaw
