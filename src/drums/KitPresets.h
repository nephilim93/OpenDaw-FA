#pragma once

#include "DrumKit.h"
#include <QString>
#include <QVector>
#include <optional>

// Kit presets: a few built-in synth kits, plus the user's own, saved as
// "<name>.drumkit" (the kit's JSON) in the Kits folder inside the samples
// folder. Saved samples inside the samples folder are relative to it, so a
// preset and its samples travel together.
namespace OpenDaw::Drums {

struct KitPreset {
    QString name;
    bool builtIn = false;
    QString file;   // user presets: the .drumkit file
};

inline constexpr const char* kKitPresetSuffix = ".drumkit";

// <samplesFolder>/Kits
QString kitsFolder(const QString& samplesFolder);
// Built-ins first, then the user's presets by name.
QVector<KitPreset> listKitPresets(const QString& samplesFolder);
// By name, ignoring case.
std::optional<KitPreset> findKitPreset(const QString& name, const QString& samplesFolder);
std::optional<Kit> loadKitPreset(const QString& name, const QString& samplesFolder, QString* error);

// Why `name` can't name a preset (empty when it can).
QString kitPresetNameError(const QString& name);
// Writes the kit as a user preset; sample paths inside the samples folder
// are stored relative. Refuses a built-in's name, and an existing preset
// unless `overwrite`. `outsideSamples` (optional) receives the pads whose
// samples live outside the samples folder (the preset only works where
// those files are).
bool saveKitPreset(const Kit& kit, const QString& name, const QString& samplesFolder, bool overwrite,
                   QString* error, QStringList* outsideSamples = nullptr);

} // namespace OpenDaw::Drums
