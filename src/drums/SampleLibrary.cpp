#include "SampleLibrary.h"
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace OpenDaw::Drums {

namespace {

// Resolves symlinks in the part of the path that exists, so /tmp and
// /private/tmp, or a symlinked folder, compare equal even for a file that
// isn't there yet.
QString normalized(const QString& path)
{
    QString existing = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString rest;
    while (!QFileInfo::exists(existing)) {
        const int slash = existing.lastIndexOf('/');
        if (slash <= 0) return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        rest = existing.mid(slash) + rest;
        existing = existing.left(slash);
    }
    return QDir::cleanPath(QFileInfo(existing).canonicalFilePath() + rest);
}

} // namespace

QString defaultSamplesFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/OpenDAW/Samples";
}

QString samplesFolder()
{
    const QString folder = QSettings().value(kSamplesFolderKey).toString().trimmed();
    return folder.isEmpty() ? defaultSamplesFolder() : folder;
}

void setSamplesFolder(const QString& folder)
{
    QSettings().setValue(kSamplesFolderKey, QDir::cleanPath(folder.trimmed()));
}

QString toKitPath(const QString& file, const QString& folder)
{
    if (file.isEmpty() || folder.isEmpty() || QDir::isRelativePath(file)) return file;
    const QString rel = QDir(normalized(folder)).relativeFilePath(normalized(file));
    if (rel.isEmpty() || rel == "." || rel == ".." || rel.startsWith("../") || QDir::isAbsolutePath(rel))
        return QDir::cleanPath(QFileInfo(file).absoluteFilePath());   // as given, not symlink-resolved
    return rel;
}

QString resolveSamplePath(const QString& kitPath, const QString& folder)
{
    if (kitPath.isEmpty() || QDir::isAbsolutePath(kitPath) || folder.isEmpty()) return kitPath;
    return QDir::cleanPath(QDir(folder).absoluteFilePath(kitPath));
}

} // namespace OpenDaw::Drums
