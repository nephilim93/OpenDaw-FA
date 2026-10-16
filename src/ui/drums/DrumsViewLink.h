#pragma once

#include "engine/AudioEngine.h"
#include <QObject>

namespace OpenDaw {

// Lets track headers and mixer strips ask for the Drums tab without knowing
// about MainWindow (they are created deep inside other views), and tells the
// sample browsers when Settings moves the samples folder.
class DrumsViewLink : public QObject {
    Q_OBJECT

public:
    static DrumsViewLink& instance();

    void requestOpen(te::AudioTrack* track) { emit openRequested(track); }
    void notifySamplesFolderChanged() { emit samplesFolderChanged(); }

signals:
    void openRequested(te::AudioTrack* track);
    void samplesFolderChanged();

private:
    DrumsViewLink() = default;
};

} // namespace OpenDaw
