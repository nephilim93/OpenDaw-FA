#include "PluginScanner.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <QDebug>

namespace OpenDaw {

PluginScanner::PluginScanner(AudioEngine& engine, QObject* parent)
    : QObject(parent), audioEngine_(engine)
{
    formats_.push_back(std::make_unique<juce::VST3PluginFormat>());
#if JUCE_PLUGINHOST_VST
    formats_.push_back(std::make_unique<juce::VSTPluginFormat>());
#endif

    scanTimer_.setInterval(0);
    connect(&scanTimer_, &QTimer::timeout, this, &PluginScanner::scanNextFile);

    loadCachedList();
}

PluginScanner::~PluginScanner()
{
    scanTimer_.stop();
}

void PluginScanner::startScan()
{
    if (scanning_)
        return;

    scanning_ = true;
    scanners_.clear();
    currentScanner_ = 0;
    filesScanned_ = 0;
    totalFiles_ = 0;

    // The dead man's pedal records the file being scanned; if a plugin crashes
    // the app, it is blacklisted and skipped on the next scan.
    auto pedal = getDeadMansPedalFile();
    pedal.getParentDirectory().createDirectory();

    for (auto& fmt : formats_) {
        auto paths = fmt->getDefaultLocationsToSearch();
        totalFiles_ += fmt->searchPathsForPlugins(paths, true, true).size();
        auto scanner = std::make_unique<juce::PluginDirectoryScanner>(
            getPluginList(), *fmt, paths, true, pedal, false);
        scanners_.push_back(std::move(scanner));
    }

    scanTimer_.start();
}

void PluginScanner::scanNextFile()
{
    if (currentScanner_ >= scanners_.size()) {
        finishScan();
        return;
    }

    auto& scanner = *scanners_[currentScanner_];
    juce::String name;
    if (!scanner.scanNextFile(true, name)) {
        ++currentScanner_;
        return;
    }

    ++filesScanned_;
    emit scanProgress(QString::fromUtf8(name.toRawUTF8()), filesScanned_, totalFiles_);
}

void PluginScanner::finishScan()
{
    scanTimer_.stop();

    for (auto& scanner : scanners_)
        for (auto& failed : scanner->getFailedFiles())
            qWarning() << "[PluginScanner] failed to load:" << failed.toRawUTF8();

    scanners_.clear();
    scanning_ = false;
    saveCachedList();
    emit scanFinished();
}

juce::File PluginScanner::getDeadMansPedalFile() const
{
    return getCacheFile().getSiblingFile("plugin-scan-pending.txt");
}

juce::KnownPluginList& PluginScanner::getPluginList()
{
    return audioEngine_.engine().getPluginManager().knownPluginList;
}

juce::File PluginScanner::getCacheFile() const
{
    auto appData = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory);
    return appData.getChildFile("OpenDaw").getChildFile("plugin-cache.xml");
}

void PluginScanner::loadCachedList()
{
    auto cacheFile = getCacheFile();
    if (!cacheFile.existsAsFile()) return;

    auto xml = juce::XmlDocument::parse(cacheFile);
    if (!xml) {
        qWarning() << "[PluginScanner] corrupt plugin cache, deleting";
        cacheFile.deleteFile();
        return;
    }
    getPluginList().recreateFromXml(*xml);

    auto& kpl = getPluginList();
    for (int i = kpl.getNumTypes() - 1; i >= 0; --i) {
        auto desc = kpl.getTypes()[i];
        // Plugins are often bundles (directories): .vst/.vst3 on macOS and
        // bundle-format .vst3 on Windows, so check exists(), not existsAsFile().
        if (!juce::File(desc.fileOrIdentifier).exists())
            kpl.removeType(desc);
    }
}

void PluginScanner::saveCachedList()
{
    auto cacheFile = getCacheFile();
    cacheFile.getParentDirectory().createDirectory();

    if (auto xml = getPluginList().createXml()) {
        xml->writeTo(cacheFile);
    }
}

} // namespace OpenDaw
