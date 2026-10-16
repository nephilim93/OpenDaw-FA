#include "KitFromFolder.h"
#include "SampleLibrary.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace OpenDaw::Drums {

namespace {

// Words of a name: "BD_Punchy01", "OpenHat", "tom-floor 2" -> bd punchy 01,
// open hat, tom floor 2. CamelCase and letter/digit boundaries split too.
QStringList words(const QString& name)
{
    QString spaced;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        if (i > 0) {
            const QChar p = name[i - 1];
            if ((p.isLower() && c.isUpper()) || (p.isLetter() && c.isDigit()) || (p.isDigit() && c.isLetter()))
                spaced += ' ';
        }
        spaced += c.isLetterOrNumber() ? c.toLower() : QChar(' ');
    }
    return spaced.split(' ', Qt::SkipEmptyParts);
}

struct Words {
    QStringList list;
    QString squashed;   // letters only, for compounds like "hihat" or "floortom"

    bool has(const char* w) const { return list.contains(QLatin1String(w)); }
    bool contains(const char* s) const { return squashed.contains(QLatin1String(s)); }
    bool any(std::initializer_list<const char*> ws) const
    {
        return std::any_of(ws.begin(), ws.end(), [this](const char* w) { return has(w); });
    }
    bool containsAny(std::initializer_list<const char*> ss) const
    {
        return std::any_of(ss.begin(), ss.end(), [this](const char* s) { return contains(s); });
    }
    int number() const
    {
        for (const auto& w : list)
            if (w[0].isDigit()) return w.toInt();
        return -1;
    }
};

Words makeWords(const QStringList& parts)
{
    Words w;
    for (const auto& p : parts) w.list += words(p);
    for (const auto& x : w.list)
        for (const QChar c : x)
            if (c.isLetter()) w.squashed += c;
    return w;
}

constexpr int kGenericTom = -2;   // a tom with no position word: placed by number later

// The note the words name, kGenericTom, or -1.
int classify(const Words& w)
{
    if (w.contains("cowbell") || w.any({ "cow", "cb" })) return Gm::Cowbell;
    if (w.contains("ridebell") || (w.contains("ride") && w.contains("bell"))) return Gm::RideBell;
    if (w.containsAny({ "tambourine", "tamb" })) return Gm::Tambourine;
    if (w.containsAny({ "shaker", "maraca" })) return 70;   // Maracas
    if (w.contains("clave")) return 75;
    if (w.contains("conga")) return w.containsAny({ "low", "lo" }) ? 64 : 63;
    if (w.contains("bongo")) return w.containsAny({ "low", "lo" }) ? 61 : 60;
    if (w.contains("triangle")) return w.containsAny({ "mute", "closed" }) ? 80 : 81;
    if (w.contains("woodblock")) return 76;

    const bool hat = w.containsAny({ "hihat", "openhat", "closedhat", "pedalhat" })
                     || w.any({ "hat", "hats", "hh", "hhat" });
    if (hat || w.any({ "oh", "ch", "ph" })) {
        if (w.contains("open") || w.any({ "oh", "opn" })) return Gm::OpenHat;
        if (w.containsAny({ "pedal", "foot" }) || w.any({ "ph", "ped" })) return Gm::PedalHat;
        return Gm::ClosedHat;
    }
    if (w.contains("clap") || w.any({ "cp", "clp" })) return Gm::Clap;
    if (w.containsAny({ "sidestick", "crossstick", "rimshot" }) || w.any({ "rim", "rims", "stick", "rs", "xstick" }))
        return Gm::SideStick;
    if (w.contains("snare") || w.any({ "sd", "snr", "sn" })) return Gm::Snare;
    if (w.containsAny({ "kick", "kik", "kck", "bassdrum" }) || w.any({ "bd", "kd" })) return Gm::Kick;

    const bool tom = w.any({ "tom", "toms", "floortom", "racktom", "hitom", "hightom", "lotom", "lowtom",
                             "midtom" });
    if (tom || w.any({ "lt", "mt", "ht", "ft" })) {
        const bool high = w.containsAny({ "high" }) || w.any({ "hi", "ht" }) || w.contains("hitom");
        if (w.contains("floor") || w.any({ "ft", "flr" })) return high ? Gm::HighFloorTom : Gm::LowFloorTom;
        if (w.contains("mid") || w.any({ "mt", "med" })) return high ? Gm::HighMidTom : Gm::LowMidTom;
        if (w.contains("low") || w.any({ "lo", "lt" }) || w.contains("lotom")) return Gm::LowTom;
        if (high) return Gm::HighTom;
        return kGenericTom;
    }

    if (w.contains("splash")) return Gm::Splash;
    if (w.containsAny({ "china", "chinese" })) return Gm::Chinese;
    if (w.contains("crash")) return Gm::Crash;
    if (w.contains("ride")) return Gm::Ride;
    if (w.containsAny({ "cymbal" }) || w.any({ "cym", "cy" })) return Gm::Crash;
    return -1;
}

// The file name decides; when it says nothing, the folders above it join
// in, nearest first ("Hats/Open 2.wav" is an open hat).
int classifyPath(const QString& relativePath, int* number = nullptr)
{
    const QFileInfo info(relativePath);
    QStringList parts{ info.completeBaseName() };
    QStringList folders = QDir::fromNativeSeparators(info.path()).split('/', Qt::SkipEmptyParts);
    folders.removeAll(".");
    for (int i = int(folders.size()); i >= 0; --i) {
        const Words w = makeWords(parts);
        const int note = classify(w);
        if (note != -1) {
            if (number) *number = makeWords({ info.completeBaseName() }).number();
            return note;
        }
        if (i == 0) break;
        parts << folders[i - 1];
    }
    return -1;
}

// Softer layers ("Snare Ghost", "Kick soft 2") are a worse first pick.
int pickPenalty(const QString& path)
{
    const Words w = makeWords({ QFileInfo(path).completeBaseName() });
    return w.any({ "soft", "ghost", "quiet", "light", "pp", "brush", "roll", "flam", "fx", "rev", "reverse" }) ? 1 : 0;
}

bool betterPick(const QString& a, const QString& b)
{
    const int pa = pickPenalty(a), pb = pickPenalty(b);
    if (pa != pb) return pa < pb;
    if (a.size() != b.size()) return a.size() < b.size();
    return a.compare(b, Qt::CaseInsensitive) < 0;
}

// Toms named only by number ("Tom 1" is the highest rack tom), spread over
// the GM toms for how many there are.
QVector<int> tomNotes(int count)
{
    switch (count) {
    case 1: return { Gm::LowMidTom };
    case 2: return { Gm::HighTom, Gm::LowTom };
    case 3: return { Gm::HighTom, Gm::LowMidTom, Gm::LowFloorTom };
    case 4: return { Gm::HighTom, Gm::LowMidTom, Gm::LowTom, Gm::LowFloorTom };
    case 5: return { Gm::HighTom, Gm::HighMidTom, Gm::LowMidTom, Gm::LowTom, Gm::LowFloorTom };
    default: return { Gm::HighTom, Gm::HighMidTom, Gm::LowMidTom, Gm::LowTom, Gm::HighFloorTom, Gm::LowFloorTom };
    }
}

} // namespace

std::optional<int> guessDrumNote(const QString& relativePath)
{
    const int note = classifyPath(relativePath);
    if (note == kGenericTom) return Gm::LowMidTom;
    if (note < 0) return std::nullopt;
    return note;
}

QStringList listSampleFiles(const QString& folder, int limit, bool* truncated)
{
    QStringList files;
    if (truncated) *truncated = false;
    const QDir root(folder);
    QDirIterator it(folder, { "*.wav", "*.aiff", "*.aif", "*.flac", "*.ogg" }, QDir::Files,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        files << root.relativeFilePath(it.next());
        if (files.size() > limit) break;   // one over: enough to know
    }
    std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    if (files.size() > limit) {
        files = files.mid(0, limit);
        if (truncated) *truncated = true;
    }
    return files;
}

FolderKit buildKitFromFolder(const QString& folder, const QString& samplesFolder, bool fillWithSynth)
{
    FolderKit out;
    const QStringList files = listSampleFiles(folder);
    out.sampleCount = int(files.size());

    QMap<int, QStringList> byNote;
    QMap<int, QStringList> numberedToms;   // tom number -> files
    for (const auto& f : files) {
        int number = -1;
        const int note = classifyPath(f, &number);
        if (note == -1) out.unmatched << f;
        else if (note == kGenericTom) numberedToms[number] << f;
        else byNote[note] << f;
    }
    // Numbered toms take positions when the numbers look like positions (a
    // few of them); otherwise they're takes of one tom.
    if (!numberedToms.isEmpty()) {
        const QList<int> numbers = numberedToms.keys();
        const bool positions = numbers.size() >= 2 && numbers.size() <= 6 && !numbers.contains(-1);
        if (positions) {
            const auto notes = tomNotes(int(numbers.size()));
            for (int i = 0; i < numbers.size(); ++i) byNote[notes[i]] += numberedToms[numbers[i]];
        } else {
            for (const auto& list : numberedToms) byNote[Gm::LowMidTom] += list;
        }
    }

    const Kit defaults = Kit::defaultKit();
    if (fillWithSynth) out.kit = defaults;
    for (auto it = byNote.begin(); it != byNote.end(); ++it) {
        auto list = it.value();
        std::sort(list.begin(), list.end(), betterPick);
        const QString file = QDir(folder).filePath(list.first());
        const QString kitPath = toKitPath(QFileInfo(file).absoluteFilePath(), samplesFolder);

        Pad pad;
        if (const auto* d = defaults.padForNote(it.key())) {
            pad = *d;   // its name, level, pan and choke
        } else {
            pad.note = it.key();
            pad.name = QFileInfo(list.first()).completeBaseName();
        }
        pad.source = PadSource::Sample;
        pad.module.clear();
        pad.params.clear();
        pad.sample = kitPath;
        pad.tune = 0.0f;
        if (auto* existing = out.kit.padForNote(pad.note)) *existing = pad;
        else out.kit.pads.append(pad);

        out.chosen[it.key()] = kitPath;
        for (int i = 1; i < list.size(); ++i)
            out.alternatives[it.key()] << toKitPath(QDir(folder).absoluteFilePath(list[i]), samplesFolder);
    }
    std::sort(out.kit.pads.begin(), out.kit.pads.end(), [](const Pad& a, const Pad& b) { return a.note < b.note; });
    return out;
}

} // namespace OpenDaw::Drums
