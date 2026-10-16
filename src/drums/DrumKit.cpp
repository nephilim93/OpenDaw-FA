#include "DrumKit.h"
#include "modular/ModuleRegistry.h"
#include "modular/Patch.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace OpenDaw::Drums {

namespace {

const Modular::ModuleDef* synthModule(const QString& type)
{
    const auto* def = Modular::findModule(type.toStdString());
    if (!def || def->inputIndex("trigger") < 0 || def->outputIndex("out") < 0) return nullptr;
    return def;
}

std::optional<PadSource> parseSource(const QString& s)
{
    if (s == "synth") return PadSource::Synth;
    if (s == "sample") return PadSource::Sample;
    return std::nullopt;
}

Pad synthPad(int note, const char* name, const char* module, QMap<QString, float> params = {},
             float level = 0.8f, float pan = 0.0f, int choke = 0)
{
    Pad p;
    p.note = note;
    p.name = name;
    p.module = module;
    p.params = std::move(params);
    p.level = level;
    p.pan = pan;
    p.choke = choke;
    return p;
}

} // namespace

const char* padSourceName(PadSource s)
{
    return s == PadSource::Sample ? "sample" : "synth";
}

QStringList synthPadModules()
{
    QStringList types;
    for (const auto& def : Modular::allModules())
        if (def.inputIndex("trigger") >= 0 && def.outputIndex("out") >= 0)
            types << QString::fromStdString(def.type);
    return types;
}

std::optional<Kit> Kit::fromJson(const QJsonObject& json, QString* error)
{
    Kit kit;
    for (const auto& value : json["pads"].toArray()) {
        const auto o = value.toObject();
        Pad p;
        p.note = o["note"].toInt(-1);
        p.name = o["name"].toString().trimmed();
        const auto source = parseSource(o["source"].toString("synth"));
        if (!source) {
            *error = QString("Pad %1: source must be 'synth' or 'sample'.").arg(p.note);
            return std::nullopt;
        }
        p.source = *source;
        p.module = o["module"].toString().trimmed();
        p.sample = o["sample"].toString();
        p.level = float(o["level"].toDouble(p.level));
        p.pan = float(o["pan"].toDouble(p.pan));
        p.tune = float(o["tune"].toDouble(p.tune));
        p.choke = o["choke"].toInt(0);
        if (p.source == PadSource::Synth) {
            const auto* def = synthModule(p.module);
            if (!def) {
                *error = QString("Pad %1: '%2' can't be a synth pad. Use one of: %3.")
                             .arg(p.note).arg(p.module, synthPadModules().join(", "));
                return std::nullopt;
            }
            const auto params = o["params"].toObject();
            for (auto it = params.begin(); it != params.end(); ++it) {
                const int pi = def->paramIndex(it.key().toStdString());
                if (pi < 0) {
                    QStringList names;
                    for (const auto& pd : def->params) names << QString::fromStdString(pd.name);
                    *error = QString("Pad %1: %2 has no parameter '%3'. Parameters: %4.")
                                 .arg(p.note).arg(p.module, it.key(), names.join(", "));
                    return std::nullopt;
                }
                QString err;
                const auto v = Modular::parseParamValue(def->params[size_t(pi)], it.value(), &err);
                if (!v) { *error = QString("Pad %1: %2").arg(p.note).arg(err); return std::nullopt; }
                p.params[it.key()] = *v;
            }
        }
        kit.pads.append(p);
    }
    const auto errors = kit.validate();
    if (!errors.isEmpty()) {
        *error = errors.join(" ");
        return std::nullopt;
    }
    return kit;
}

QJsonObject Kit::toJson() const
{
    QJsonArray pads;
    for (const auto& p : this->pads) {
        QJsonObject o{ { "note", p.note }, { "name", p.name }, { "source", padSourceName(p.source) },
                       { "level", double(p.level) } };
        if (p.pan != 0.0f) o["pan"] = double(p.pan);
        if (p.tune != 0.0f) o["tune"] = double(p.tune);
        if (p.choke != 0) o["choke"] = p.choke;
        if (p.source == PadSource::Synth) {
            o["module"] = p.module;
            const auto* def = synthModule(p.module);
            QJsonObject params;
            for (auto it = p.params.begin(); it != p.params.end(); ++it) {
                const int pi = def ? def->paramIndex(it.key().toStdString()) : -1;
                params[it.key()] = pi >= 0 ? Modular::paramValueToJson(def->params[size_t(pi)], it.value())
                                           : QJsonValue(double(it.value()));
            }
            if (!params.isEmpty()) o["params"] = params;
        } else {
            o["sample"] = p.sample;
        }
        pads.append(o);
    }
    return { { "pads", pads } };
}

QStringList Kit::validate() const
{
    QStringList errors;
    QSet<int> notes;
    for (const auto& p : pads) {
        if (p.note < 0 || p.note > 127)
            errors << QString("Pad note %1 is outside MIDI's 0..127.").arg(p.note);
        else if (notes.contains(p.note))
            errors << QString("Two pads play note %1; each note can have one pad.").arg(p.note);
        notes.insert(p.note);
        if (p.level < 0.0f || p.level > 1.5f)
            errors << QString("Pad %1: level %2 is out of range (0 to 1.5).").arg(p.note).arg(p.level);
        if (p.pan < -1.0f || p.pan > 1.0f)
            errors << QString("Pad %1: pan %2 is out of range (-1 to 1).").arg(p.note).arg(p.pan);
        if (p.tune < -24.0f || p.tune > 24.0f)
            errors << QString("Pad %1: tune %2 is out of range (-24 to 24 semitones).").arg(p.note).arg(p.tune);
        if (p.choke < 0 || p.choke > 16)
            errors << QString("Pad %1: choke group %2 is out of range (0 = none, 1 to 16).").arg(p.note).arg(p.choke);
        if (p.source == PadSource::Synth && !synthModule(p.module))
            errors << QString("Pad %1: '%2' can't be a synth pad. Use one of: %3.")
                          .arg(p.note).arg(p.module, synthPadModules().join(", "));
        if (p.source == PadSource::Sample && p.sample.trimmed().isEmpty())
            errors << QString("Pad %1 is a sample pad without a sample file.").arg(p.note);
    }
    return errors;
}

const Pad* Kit::padForNote(int note) const
{
    for (const auto& p : pads)
        if (p.note == note) return &p;
    return nullptr;
}

Pad* Kit::padForNote(int note)
{
    for (auto& p : pads)
        if (p.note == note) return &p;
    return nullptr;
}

Kit Kit::defaultKit()
{
    // Hats share choke group 1, so a closed or pedal hat cuts off an open one.
    // Toms are spread from floor (left) to high (right), as seen from the drummer's seat.
    auto tom = [](int note, const char* name, float hz, float pan) {
        return synthPad(note, name, "tom", { { "tune", hz }, { "decay", 0.25f + 30.0f / hz } }, 0.75f, pan);
    };
    Kit kit;
    kit.pads = {
        synthPad(Gm::Kick, "Kick", "kick", {}, 0.9f),
        synthPad(Gm::SideStick, "Side Stick", "rimshot", {}, 0.6f),
        synthPad(Gm::Snare, "Snare", "snare", {}, 0.8f),
        synthPad(Gm::Clap, "Clap", "clap", {}, 0.7f),
        synthPad(Gm::ElectricSnare, "Electric Snare", "snare",
                 { { "tune", 220.0f }, { "snappy", 0.8f }, { "color", 3000.0f } }, 0.75f),
        tom(Gm::LowFloorTom, "Low Floor Tom", 75.0f, -0.4f),
        synthPad(Gm::ClosedHat, "Closed Hat", "hihat", { { "decay", 0.05f } }, 0.55f, 0.2f, 1),
        tom(Gm::HighFloorTom, "High Floor Tom", 90.0f, -0.3f),
        synthPad(Gm::PedalHat, "Pedal Hat", "hihat", { { "decay", 0.03f }, { "color", 6000.0f } }, 0.45f, 0.2f, 1),
        tom(Gm::LowTom, "Low Tom", 110.0f, -0.15f),
        synthPad(Gm::OpenHat, "Open Hat", "hihat", { { "decay", 0.6f } }, 0.5f, 0.2f, 1),
        tom(Gm::LowMidTom, "Low-Mid Tom", 135.0f, 0.0f),
        tom(Gm::HighMidTom, "High-Mid Tom", 165.0f, 0.15f),
        synthPad(Gm::Crash, "Crash", "cymbal", { { "decay", 2.0f } }, 0.5f, -0.3f),
        tom(Gm::HighTom, "High Tom", 200.0f, 0.3f),
        synthPad(Gm::Ride, "Ride", "cymbal", { { "kind", 1.0f }, { "decay", 2.5f } }, 0.45f, 0.35f),
        synthPad(Gm::RideBell, "Ride Bell", "cymbal",
                 { { "kind", 1.0f }, { "decay", 2.0f }, { "bell", 1.0f } }, 0.45f, 0.35f),
        synthPad(Gm::Tambourine, "Tambourine", "hihat",
                 { { "decay", 0.15f }, { "color", 7000.0f }, { "metal", 0.9f } }, 0.4f, -0.2f),
        synthPad(Gm::Cowbell, "Cowbell", "cowbell", {}, 0.5f, 0.1f),
        synthPad(Gm::Crash2, "Crash 2", "cymbal", { { "decay", 2.4f }, { "color", 3500.0f } }, 0.5f, 0.4f),
    };
    return kit;
}

} // namespace OpenDaw::Drums
