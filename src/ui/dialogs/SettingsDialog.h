#pragma once

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <functional>

namespace OpenDaw {

class AudioEngine;
class EditManager;
class PluginScanner;

// The application Settings window: one page per area, picked from a list on
// the left. Every change applies immediately.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    enum class Page { Audio, Midi, Ai, Plugins, System };

    SettingsDialog(AudioEngine& audio, EditManager& editMgr, PluginScanner& scanner,
                   std::function<void()> rescanPlugins, QWidget* parent = nullptr);

    void showPage(Page page);

private:
    QWidget* createMidiPage();
    QWidget* createAiPage();
    QWidget* createPluginsPage();
    QWidget* createSystemPage();
    void addPage(const QString& title, QWidget* page);

    AudioEngine& audio_;
    EditManager& editMgr_;
    PluginScanner& scanner_;
    std::function<void()> rescanPlugins_;

    QListWidget* pageList_ = nullptr;
    QStackedWidget* pages_ = nullptr;
};

} // namespace OpenDaw
