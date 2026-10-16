#pragma once

#include "engine/AudioEngine.h"
#include <QObject>

namespace OpenDaw {

// Lets track headers and mixer strips ask for the Drums tab without knowing
// about MainWindow (they are created deep inside other views).
class DrumsViewLink : public QObject {
    Q_OBJECT

public:
    static DrumsViewLink& instance();

    void requestOpen(te::AudioTrack* track) { emit openRequested(track); }

signals:
    void openRequested(te::AudioTrack* track);

private:
    DrumsViewLink() = default;
};

} // namespace OpenDaw
