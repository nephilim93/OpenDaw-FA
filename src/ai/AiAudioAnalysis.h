#pragma once

#include "engine/EditManager.h"
#include <QJsonObject>
#include <memory>
#include <unordered_map>

namespace OpenDaw {

class AiAudioAnalysis {
public:
    explicit AiAudioAnalysis(EditManager* editMgr);
    ~AiAudioAnalysis();

    QJsonObject analyzeTrackLevels(te::AudioTrack* track, double windowSeconds);
    QJsonObject analyzeMasterLevels(double windowSeconds);
    QJsonObject analyzeFrequencyBalance(te::AudioTrack* track);
    QJsonObject analyzeMasterFrequencyBalance();
    QJsonObject analyzeStereoImage(te::AudioTrack* track);
    QJsonObject analyzeMasterStereoImage();
    QJsonObject analyzeTransients(te::AudioTrack* track);
    QJsonObject analyzeMasking(te::AudioTrack* trackA, te::AudioTrack* trackB);

    static QString inferRoleFromName(const QString& trackName);

private:
    struct MeterClientEntry {
        std::unique_ptr<te::LevelMeasurer::Client> client;
        // Held, so removing the client is safe after its track is deleted
        // (an undo can keep the plugin alive, still measuring).
        juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> plugin;
    };

    MeterClientEntry* ensureClient(te::AudioTrack* track);
    std::pair<float, float> readTrackLevels(te::AudioTrack* track);
    static double linearToDb(double linear);
    static double clamp01(double v);

    void dropAllClients();

    EditManager* editMgr_ = nullptr;
    std::unordered_map<qulonglong, MeterClientEntry> meterClients_;
    QMetaObject::Connection tracksConn_, editConn_;   // disconnected in the destructor
};

} // namespace OpenDaw

