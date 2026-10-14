#include "Theory.h"
#include <QMap>
#include <algorithm>

namespace OpenDaw::Theory {

namespace {

// Reads a note letter and its accidentals from the front of `text`.
// Returns the pitch class and how many characters were used.
std::optional<std::pair<int, int>> readPitchClass(const QString& text)
{
    if (text.isEmpty()) return std::nullopt;

    static const int letters[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    const QChar first = text[0].toUpper();
    if (first < 'A' || first > 'G') return std::nullopt;
    int pc = letters[first.unicode() - 'A'];

    int used = 1;
    while (used < text.size()) {
        const QChar c = text[used];
        if (c == '#' || c == QChar(0x266F)) pc += 1;
        else if (c == 'b' || c == QChar(0x266D)) pc -= 1;
        else break;
        ++used;
    }
    return std::make_pair(((pc % 12) + 12) % 12, used);
}

QVector<int> chordQuality(const QString& suffix)
{
    static const QMap<QString, QVector<int>> qualities = {
        { "",      { 0, 4, 7 } },
        { "maj",   { 0, 4, 7 } },
        { "M",     { 0, 4, 7 } },
        { "m",     { 0, 3, 7 } },
        { "min",   { 0, 3, 7 } },
        { "-",     { 0, 3, 7 } },
        { "dim",   { 0, 3, 6 } },
        { "o",     { 0, 3, 6 } },
        { "°",     { 0, 3, 6 } },
        { "aug",   { 0, 4, 8 } },
        { "+",     { 0, 4, 8 } },
        { "sus2",  { 0, 2, 7 } },
        { "sus4",  { 0, 5, 7 } },
        { "sus",   { 0, 5, 7 } },
        { "5",     { 0, 7 } },
        { "6",     { 0, 4, 7, 9 } },
        { "m6",    { 0, 3, 7, 9 } },
        { "7",     { 0, 4, 7, 10 } },
        { "maj7",  { 0, 4, 7, 11 } },
        { "M7",    { 0, 4, 7, 11 } },
        { "Δ",     { 0, 4, 7, 11 } },
        { "Δ7",    { 0, 4, 7, 11 } },
        { "m7",    { 0, 3, 7, 10 } },
        { "min7",  { 0, 3, 7, 10 } },
        { "-7",    { 0, 3, 7, 10 } },
        { "mmaj7", { 0, 3, 7, 11 } },
        { "mM7",   { 0, 3, 7, 11 } },
        { "m7b5",  { 0, 3, 6, 10 } },
        { "ø",     { 0, 3, 6, 10 } },
        { "ø7",    { 0, 3, 6, 10 } },
        { "dim7",  { 0, 3, 6, 9 } },
        { "o7",    { 0, 3, 6, 9 } },
        { "°7",    { 0, 3, 6, 9 } },
        { "aug7",  { 0, 4, 8, 10 } },
        { "+7",    { 0, 4, 8, 10 } },
        { "7sus4", { 0, 5, 7, 10 } },
        { "add9",  { 0, 4, 7, 14 } },
        { "madd9", { 0, 3, 7, 14 } },
        { "9",     { 0, 4, 7, 10, 14 } },
        { "m9",    { 0, 3, 7, 10, 14 } },
        { "maj9",  { 0, 4, 7, 11, 14 } },
    };
    return qualities.value(suffix);
}

// Roman numeral chords relative to `key`: case sets major or minor, then an
// optional °, ø or + and an optional 7.
std::optional<Chord> parseRoman(const QString& symbol, const Scale& key)
{
    QString s = symbol;
    int accidental = 0;
    while (!s.isEmpty() && (s[0] == 'b' || s[0] == '#')) {
        accidental += s[0] == '#' ? 1 : -1;
        s.remove(0, 1);
    }

    static const QStringList numerals = { "VII", "III", "VI", "IV", "II", "V", "I" };
    static const QMap<QString, int> degrees = {
        { "I", 0 }, { "II", 1 }, { "III", 2 }, { "IV", 3 }, { "V", 4 }, { "VI", 5 }, { "VII", 6 } };

    for (const auto& numeral : numerals) {
        if (!s.startsWith(numeral, Qt::CaseInsensitive)) continue;
        const QString written = s.left(numeral.size());
        const bool upper = written == numeral;
        if (!upper && written != numeral.toLower()) continue; // mixed case

        const int degree = degrees.value(numeral);
        if (degree >= key.intervals.size()) return std::nullopt;

        QString rest = s.mid(numeral.size());
        QVector<int> intervals;
        if (rest == "°7" || rest == "o7")           intervals = { 0, 3, 6, 9 };
        else if (rest == "ø" || rest == "ø7")       intervals = { 0, 3, 6, 10 };
        else if (rest == "°" || rest == "o")        intervals = { 0, 3, 6 };
        else if (rest == "+")                       intervals = { 0, 4, 8 };
        else if (rest == "maj7" || rest == "M7")    intervals = { 0, 4, 7, 11 };
        else if (rest == "7")                       intervals = upper ? QVector<int>{ 0, 4, 7, 10 }
                                                                      : QVector<int>{ 0, 3, 7, 10 };
        else if (rest.isEmpty())                    intervals = upper ? QVector<int>{ 0, 4, 7 }
                                                                      : QVector<int>{ 0, 3, 7 };
        else return std::nullopt;

        Chord chord;
        chord.root = (key.root + key.intervals[degree] + accidental + 24) % 12;
        chord.intervals = intervals;
        chord.symbol = symbol;
        return chord;
    }
    return std::nullopt;
}

} // namespace

std::optional<int> parsePitchClass(const QString& name)
{
    const QString s = name.trimmed();
    auto read = readPitchClass(s);
    if (!read || read->second != s.size()) return std::nullopt;
    return read->first;
}

std::optional<int> parseNote(const QString& name)
{
    const QString s = name.trimmed();
    auto read = readPitchClass(s);
    if (!read) return std::nullopt;

    // The written letter decides the octave (B#3 is C4, Cb4 is B3).
    static const int naturals[] = { 9, 11, 0, 2, 4, 5, 7 };
    const int natural = naturals[s[0].toUpper().unicode() - 'A'];
    int offset = read->first - natural;
    if (offset > 6) offset -= 12;
    if (offset < -6) offset += 12;

    bool okOctave = false;
    const int octave = s.mid(read->second).toInt(&okOctave);
    if (!okOctave) return std::nullopt;

    const int midi = (octave + 1) * 12 + natural + offset;
    if (midi < 0 || midi > 127) return std::nullopt;
    return midi;
}

QString noteName(int midiNote)
{
    static const char* names[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    return QString("%1%2").arg(names[((midiNote % 12) + 12) % 12]).arg(midiNote / 12 - 1);
}

bool Scale::contains(int midiNote) const
{
    const int pc = ((midiNote - root) % 12 + 12) % 12;
    return intervals.contains(pc);
}

int Scale::snap(int midiNote) const
{
    for (int distance = 0; distance <= 6; ++distance) {
        if (contains(midiNote - distance)) return midiNote - distance;
        if (contains(midiNote + distance)) return midiNote + distance;
    }
    return midiNote;
}

std::optional<Scale> parseScale(const QString& key)
{
    QString s = key.trimmed().simplified();
    auto read = readPitchClass(s);
    if (!read) return std::nullopt;

    QString mode = s.mid(read->second).trimmed().toLower();
    if (mode.startsWith("-")) mode.remove(0, 1);

    static const QMap<QString, QPair<QString, QVector<int>>> modes = {
        { "",                 { "major",            { 0, 2, 4, 5, 7, 9, 11 } } },
        { "major",            { "major",            { 0, 2, 4, 5, 7, 9, 11 } } },
        { "maj",              { "major",            { 0, 2, 4, 5, 7, 9, 11 } } },
        { "ionian",           { "major",            { 0, 2, 4, 5, 7, 9, 11 } } },
        { "m",                { "natural minor",    { 0, 2, 3, 5, 7, 8, 10 } } },
        { "min",              { "natural minor",    { 0, 2, 3, 5, 7, 8, 10 } } },
        { "minor",            { "natural minor",    { 0, 2, 3, 5, 7, 8, 10 } } },
        { "natural minor",    { "natural minor",    { 0, 2, 3, 5, 7, 8, 10 } } },
        { "aeolian",          { "natural minor",    { 0, 2, 3, 5, 7, 8, 10 } } },
        { "harmonic minor",   { "harmonic minor",   { 0, 2, 3, 5, 7, 8, 11 } } },
        { "melodic minor",    { "melodic minor",    { 0, 2, 3, 5, 7, 9, 11 } } },
        { "dorian",           { "dorian",           { 0, 2, 3, 5, 7, 9, 10 } } },
        { "phrygian",         { "phrygian",         { 0, 1, 3, 5, 7, 8, 10 } } },
        { "lydian",           { "lydian",           { 0, 2, 4, 6, 7, 9, 11 } } },
        { "mixolydian",       { "mixolydian",       { 0, 2, 4, 5, 7, 9, 10 } } },
        { "locrian",          { "locrian",          { 0, 1, 3, 5, 6, 8, 10 } } },
        { "major pentatonic", { "major pentatonic", { 0, 2, 4, 7, 9 } } },
        { "pentatonic",       { "major pentatonic", { 0, 2, 4, 7, 9 } } },
        { "minor pentatonic", { "minor pentatonic", { 0, 3, 5, 7, 10 } } },
        { "blues",            { "blues",            { 0, 3, 5, 6, 7, 10 } } },
        { "chromatic",        { "chromatic",        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } } },
    };
    if (!modes.contains(mode)) return std::nullopt;

    Scale scale;
    scale.root = read->first;
    scale.intervals = modes.value(mode).second;
    scale.name = s.left(read->second) + " " + modes.value(mode).first;
    return scale;
}

std::optional<Chord> parseChord(const QString& symbol, const std::optional<Scale>& key)
{
    QString s = symbol.trimmed();
    if (s.isEmpty()) return std::nullopt;

    // Slash chords: the bass note is ignored.
    const int slash = s.indexOf('/');
    if (slash > 0) s = s.left(slash);

    // Roman numerals first: "VI" would otherwise not parse, but a lone "b"
    // could be read as B, so only try them when a key is known.
    if (key) {
        if (auto roman = parseRoman(s, *key)) return roman;
    }

    auto read = readPitchClass(s);
    if (!read) return std::nullopt;

    const QString suffix = s.mid(read->second);
    const QVector<int> intervals = chordQuality(suffix);
    if (intervals.isEmpty()) return std::nullopt;

    Chord chord;
    chord.root = read->first;
    chord.intervals = intervals;
    chord.symbol = symbol.trimmed();
    return chord;
}

} // namespace OpenDaw::Theory
