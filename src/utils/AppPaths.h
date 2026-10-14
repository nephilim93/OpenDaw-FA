#pragma once

#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace OpenDaw {

// Folder for OpenDaw.log. Windows keeps it next to the executable. Elsewhere
// the app folder is not the place for it: writing inside a macOS bundle
// breaks its code signature, and an AppImage's folder is read-only.
inline QString logDirectory()
{
#ifdef _WIN32
    return QApplication::applicationDirPath();
#elif defined(__APPLE__)
    return QDir::homePath() + "/Library/Logs/OpenDaw";
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
}

} // namespace OpenDaw
