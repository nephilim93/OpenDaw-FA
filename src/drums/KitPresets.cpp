#include "KitPresets.h"
#include "SampleLibrary.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace OpenDaw::Drums {

namespace {

void setParams(Kit& kit, int note, QMap<QString, float> params, float level = -1.0f)
{
    if (auto* p = kit.padForNote(note)) {
        for (auto it = params.begin(); it != params.end(); ++it) p->params[it.key()] = it.value();
        if (level >= 0.0f) p->level = level;
    }
}

// Long, boomy kick and soft, noisy hats, after the TR-808.
Kit synth808()
{
    Kit kit = Kit::defaultKit();
    setParams(kit, Gm::Kick, { { "tune", 45.0f }, { "sweep", 2.0f }, { "pitch_decay", 0.06f },
                               { "decay", 1.6f }, { "click", 0.05f }, { "drive", 0.1f } }, 0.95f);
    setParams(kit, Gm::Snare, { { "tune", 200.0f }, { "tone_decay", 0.12f }, { "noise_decay", 0.15f },
                                { "snappy", 0.5f }, { "color", 1500.0f } });
    setParams(kit, Gm::Clap, { { "tone", 1000.0f }, { "decay", 0.4f } });
    setParams(kit, Gm::ClosedHat, { { "decay", 0.04f }, { "metal", 0.85f }, { "color", 9000.0f } });
    setParams(kit, Gm::PedalHat, { { "decay", 0.03f }, { "metal", 0.85f }, { "color", 9000.0f } });
    setParams(kit, Gm::OpenHat, { { "decay", 0.45f }, { "metal", 0.85f }, { "color", 9000.0f } });
    for (int tom : { Gm::LowFloorTom, Gm::HighFloorTom, Gm::LowTom, Gm::LowMidTom, Gm::HighMidTom, Gm::HighTom })
        setParams(kit, tom, { { "sweep", 1.0f }, { "attack", 0.0f } });
    setParams(kit, Gm::Cowbell, {}, 0.6f);
    return kit;
}

// Punchy, driven kick and bright, noisy hats, after the TR-909.
Kit synth909()
{
    Kit kit = Kit::defaultKit();
    setParams(kit, Gm::Kick, { { "tune", 55.0f }, { "sweep", 4.0f }, { "pitch_decay", 0.025f },
                               { "decay", 0.45f }, { "click", 0.6f }, { "drive", 0.5f } }, 0.95f);
    setParams(kit, Gm::Snare, { { "tune", 190.0f }, { "tone_decay", 0.1f }, { "noise_decay", 0.25f },
                                { "snappy", 0.75f }, { "color", 3000.0f } });
    setParams(kit, Gm::Clap, { { "tone", 1400.0f }, { "spread", 0.012f }, { "decay", 0.3f } });
    setParams(kit, Gm::ClosedHat, { { "decay", 0.06f }, { "metal", 0.3f }, { "color", 7000.0f } });
    setParams(kit, Gm::PedalHat, { { "decay", 0.04f }, { "metal", 0.3f }, { "color", 7000.0f } });
    setParams(kit, Gm::OpenHat, { { "decay", 0.5f }, { "metal", 0.3f }, { "color", 7000.0f } });
    setParams(kit, Gm::Crash, { { "decay", 2.5f }, { "color", 5000.0f } });
    setParams(kit, Gm::Ride, { { "decay", 3.0f }, { "color", 6000.0f } });
    return kit;
}

struct BuiltIn {
    const char* name;
    Kit (*make)();
};

const BuiltIn kBuiltIns[] = {
    { "Synth Kit", &Kit::defaultKit },
    { "Synth 808", &synth808 },
    { "Synth 909", &synth909 },
};

QString presetFile(const QString& name, const QString& samplesFolder)
{
    return QDir(kitsFolder(samplesFolder)).filePath(name + kKitPresetSuffix);
}

} // namespace

QString kitsFolder(const QString& samplesFolder)
{
    return QDir(samplesFolder).filePath("Kits");
}

QVector<KitPreset> listKitPresets(const QString& samplesFolder)
{
    QVector<KitPreset> presets;
    for (const auto& b : kBuiltIns) presets.append({ b.name, true, {} });

    QVector<KitPreset> user;
    const QDir dir(kitsFolder(samplesFolder));
    for (const auto& info : dir.entryInfoList({ QString("*") + kKitPresetSuffix }, QDir::Files)) {
        const QString name = info.fileName().chopped(int(qstrlen(kKitPresetSuffix)));
        const bool shadowed = std::any_of(presets.begin(), presets.end(), [&](const KitPreset& p) {
            return p.name.compare(name, Qt::CaseInsensitive) == 0;
        });
        if (!shadowed) user.append({ name, false, info.absoluteFilePath() });
    }
    std::sort(user.begin(), user.end(), [](const KitPreset& a, const KitPreset& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    presets += user;
    return presets;
}

std::optional<KitPreset> findKitPreset(const QString& name, const QString& samplesFolder)
{
    for (const auto& p : listKitPresets(samplesFolder))
        if (p.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0) return p;
    return std::nullopt;
}

std::optional<Kit> loadKitPreset(const QString& name, const QString& samplesFolder, QString* error)
{
    const auto preset = findKitPreset(name, samplesFolder);
    if (!preset) {
        QStringList names;
        for (const auto& p : listKitPresets(samplesFolder)) names << p.name;
        *error = QString("There is no kit preset '%1'. Presets: %2.").arg(name.trimmed(), names.join(", "));
        return std::nullopt;
    }
    if (preset->builtIn)
        for (const auto& b : kBuiltIns)
            if (preset->name == b.name) return b.make();

    QFile file(preset->file);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("Can't read %1.").arg(preset->file);
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        *error = QString("%1 isn't a kit (%2).").arg(QFileInfo(preset->file).fileName(), parseError.errorString());
        return std::nullopt;
    }
    QString kitError;
    auto kit = Kit::fromJson(doc.object(), &kitError);
    if (!kit) *error = QString("%1: %2").arg(QFileInfo(preset->file).fileName(), kitError);
    return kit;
}

QString kitPresetNameError(const QString& name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) return "A kit preset needs a name.";
    if (n.size() > 64) return "Kit preset names are at most 64 characters.";
    static const QString kBad = "/\\:*?\"<>|";
    for (const QChar c : n)
        if (kBad.contains(c) || c.unicode() < 32)
            return QString("A kit preset name can't contain %1.").arg(c.unicode() < 32 ? QString("control characters") : QString("'%1'").arg(c));
    if (n.startsWith('.')) return "A kit preset name can't start with '.'.";
    return {};
}

bool saveKitPreset(const Kit& kit, const QString& name, const QString& samplesFolder, bool overwrite,
                   QString* error, QStringList* outsideSamples)
{
    const QString n = name.trimmed();
    if (const QString e = kitPresetNameError(n); !e.isEmpty()) { *error = e; return false; }
    if (const auto existing = findKitPreset(n, samplesFolder)) {
        if (existing->builtIn) {
            *error = QString("'%1' is a built-in kit; save under another name.").arg(existing->name);
            return false;
        }
        if (!overwrite) {
            *error = QString("A kit preset named '%1' already exists.").arg(existing->name);
            return false;
        }
    }
    if (const auto errors = kit.validate(); !errors.isEmpty()) {
        *error = errors.join(" ");
        return false;
    }

    Kit portable = kit;
    for (auto& pad : portable.pads) {
        if (pad.source != PadSource::Sample) continue;
        pad.sample = toKitPath(resolveSamplePath(pad.sample, samplesFolder), samplesFolder);
        if (outsideSamples && QDir::isAbsolutePath(pad.sample))
            *outsideSamples << QString("%1 (%2)").arg(pad.name, QFileInfo(pad.sample).fileName());
    }

    if (!QDir().mkpath(kitsFolder(samplesFolder))) {
        *error = QString("Can't create %1.").arg(kitsFolder(samplesFolder));
        return false;
    }
    // An existing preset that differs only in case is replaced, not doubled.
    QString path = presetFile(n, samplesFolder);
    if (const auto existing = findKitPreset(n, samplesFolder); existing && !existing->builtIn && existing->file != path) {
        QFile::remove(existing->file);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(portable.toJson()).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        *error = QString("Can't write %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace OpenDaw::Drums
