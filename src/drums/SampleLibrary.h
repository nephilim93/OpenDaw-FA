#pragma once

#include <QString>

// The user's samples folder, and how kits refer to the files in it. A sample
// inside the folder is stored relative to it ("808/kick.wav", always with '/'),
// so a kit or a project keeps working when the folder moves or the project
// opens on another machine; a sample anywhere else keeps its absolute path.
namespace OpenDaw::Drums {

inline constexpr const char* kSamplesFolderKey = "paths/samplesFolder";

// Music/OpenDAW/Samples.
QString defaultSamplesFolder();
// The folder set in Settings, or the default.
QString samplesFolder();
void setSamplesFolder(const QString& folder);

// The path a kit stores for `file`: relative when it is inside `folder`.
QString toKitPath(const QString& file, const QString& folder);
// The file a kit path names: relative paths resolve against `folder`.
QString resolveSamplePath(const QString& kitPath, const QString& folder);
inline QString resolveSamplePath(const QString& kitPath) { return resolveSamplePath(kitPath, samplesFolder()); }

} // namespace OpenDaw::Drums
