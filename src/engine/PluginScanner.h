#pragma once

#include "AudioEngine.h"
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

namespace OpenDaw {

// Scans plugins on the message thread (plugins such as Kontakt initialise
// GUI frameworks on load and crash if loaded off the main thread). Work is
// split into one file per timer tick so the UI stays responsive.
class PluginScanner : public QObject {
    Q_OBJECT

public:
    explicit PluginScanner(AudioEngine& engine, QObject* parent = nullptr);
    ~PluginScanner() override;

    void startScan();
    bool isScanning() const { return scanning_; }
    juce::KnownPluginList& getPluginList();

    // Extra folders (from Settings) scanned in addition to the defaults.
    static QStringList extraFolders();
    static void setExtraFolders(const QStringList& folders);
    juce::StringArray defaultFolders() const;
    void clearBlacklist();

    void loadCachedList();
    void saveCachedList();
    juce::File getCacheFile() const;

signals:
    void scanProgress(const QString& pluginName, int current, int total);
    void scanFinished();

private:
    void scanNextFile();
    void finishScan();
    juce::File getDeadMansPedalFile() const;

    AudioEngine& audioEngine_;
    QTimer scanTimer_;
    std::vector<std::unique_ptr<juce::AudioPluginFormat>> formats_;
    std::vector<std::unique_ptr<juce::PluginDirectoryScanner>> scanners_;
    size_t currentScanner_ = 0;
    int filesScanned_ = 0;
    int totalFiles_ = 0;
    bool scanning_ = false;
};

} // namespace OpenDaw
