#pragma once

#include "engine/AudioEngine.h"
#include <QObject>

namespace OpenDaw {

// Lets track headers and mixer strips ask for the Modular editor without
// knowing about MainWindow (they are created deep inside other views).
class ModularEditorLink : public QObject {
    Q_OBJECT

public:
    static ModularEditorLink& instance();

    void requestOpen(te::AudioTrack* track) { emit openRequested(track); }

signals:
    void openRequested(te::AudioTrack* track);

private:
    ModularEditorLink() = default;
};

} // namespace OpenDaw
