#include "Patch.h"
#include <QSet>
#include <algorithm>

namespace OpenDaw::Modular {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString listNames(const std::vector<PortDef>& ports)
{
    QStringList names;
    for (const auto& p : ports) names << q(p.name);
    return names.isEmpty() ? QString("(none)") : names.join(", ");
}

QString listParams(const ModuleDef& def)
{
    QStringList names;
    for (const auto& p : def.params) names << q(p.name);
    return names.isEmpty() ? QString("(none)") : names.join(", ");
}

QString listTypes()
{
    QStringList names;
    for (const auto& d : allModules()) names << q(d.type);
    return names.join(", ");
}

// A parameter value from JSON: a number, or a choice name for enum params.
std::optional<float> parseParamValue(const ParamDef& def, const QJsonValue& v, QString* error)
{
    if (!def.choices.empty() && v.isString()) {
        for (size_t i = 0; i < def.choices.size(); ++i)
            if (v.toString().compare(q(def.choices[i]), Qt::CaseInsensitive) == 0)
                return static_cast<float>(i);
        QStringList options;
        for (const auto& c : def.choices) options << q(c);
        *error = QString("'%1' is not a valid %2. Use one of: %3.")
                     .arg(v.toString(), q(def.name), options.join(", "));
        return std::nullopt;
    }
    if (!v.isDouble()) {
        *error = QString("Parameter '%1' needs a number").arg(q(def.name))
                 + (def.choices.empty() ? "." : " or a choice name.");
        return std::nullopt;
    }
    const float value = static_cast<float>(v.toDouble());
    if (value < def.min - 1e-4f || value > def.max + 1e-4f) {
        *error = QString("%1 = %2 is out of range (%3 to %4%5).")
                     .arg(q(def.name)).arg(value).arg(def.min).arg(def.max)
                     .arg(def.unit.empty() ? QString() : " " + q(def.unit));
        return std::nullopt;
    }
    return def.choices.empty() ? value : std::round(value);
}

QJsonValue paramValueToJson(const ParamDef& def, float value)
{
    if (!def.choices.empty()) {
        const int i = std::clamp(static_cast<int>(value), 0, int(def.choices.size()) - 1);
        return q(def.choices[size_t(i)]);
    }
    return value;
}

std::optional<Section> parseSection(const QString& s)
{
    if (s == "voice") return Section::Voice;
    if (s == "global") return Section::Global;
    return std::nullopt;
}

Section defaultSection(const ModuleDef& def)
{
    return def.section == Section::Global ? Section::Global : Section::Voice;
}

} // namespace

std::optional<PortRef> parsePortRef(const QString& text)
{
    const int dot = text.lastIndexOf('.');
    if (dot <= 0 || dot == text.size() - 1) return std::nullopt;
    return PortRef{ text.left(dot).trimmed(), text.mid(dot + 1).trimmed() };
}

const PatchModule* Patch::find(const QString& id) const
{
    for (const auto& m : modules) if (m.id == id) return &m;
    return nullptr;
}

PatchModule* Patch::find(const QString& id)
{
    for (auto& m : modules) if (m.id == id) return &m;
    return nullptr;
}

float Patch::paramValue(const PatchModule& m, int paramIndex) const
{
    const auto* def = findModule(m.type.toStdString());
    if (!def || paramIndex < 0 || paramIndex >= int(def->params.size())) return 0.0f;
    const auto& pd = def->params[size_t(paramIndex)];
    return m.params.value(q(pd.name), pd.defaultValue);
}

// ── JSON ───────────────────────────────────────────────────────────────────

std::optional<Patch> Patch::fromJson(const QJsonObject& json, QString* error)
{
    Patch patch;
    QString err;

    for (const auto& mv : json["modules"].toArray()) {
        const auto mo = mv.toObject();
        PatchModule m;
        m.id = mo["id"].toString().trimmed();
        m.type = mo["type"].toString().trimmed();
        const auto* def = findModule(m.type.toStdString());
        if (!def) {
            *error = QString("Unknown module type '%1'. Available: %2.").arg(m.type, listTypes());
            return std::nullopt;
        }
        if (mo.contains("section")) {
            auto s = parseSection(mo["section"].toString());
            if (!s) {
                *error = QString("Module '%1': section must be 'voice' or 'global'.").arg(m.id);
                return std::nullopt;
            }
            m.section = *s;
        } else {
            m.section = defaultSection(*def);
        }
        const auto params = mo["params"].toObject();
        for (auto it = params.begin(); it != params.end(); ++it) {
            const int pi = def->paramIndex(it.key().toStdString());
            if (pi < 0) {
                *error = QString("Module '%1' (%2) has no parameter '%3'. Parameters: %4.")
                             .arg(m.id, m.type, it.key(), listParams(*def));
                return std::nullopt;
            }
            auto value = parseParamValue(def->params[size_t(pi)], it.value(), &err);
            if (!value) { *error = QString("Module '%1': %2").arg(m.id, err); return std::nullopt; }
            m.params[it.key()] = *value;
        }
        m.x = float(mo["x"].toDouble(0.0));
        m.y = float(mo["y"].toDouble(0.0));
        patch.modules.append(m);
    }

    for (const auto& cv : json["cables"].toArray()) {
        const auto co = cv.toObject();
        auto from = parsePortRef(co["from"].toString());
        auto to = parsePortRef(co["to"].toString());
        if (!from || !to) {
            *error = "Cables need \"from\" and \"to\" as \"module.port\", e.g. "
                     "{\"from\": \"vco1.out\", \"to\": \"vcf1.in\"}.";
            return std::nullopt;
        }
        patch.cables.append({ *from, *to });
    }

    const auto macros = json["macros"].toArray();
    for (int i = 0; i < std::min(int(macros.size()), kNumMacros); ++i) {
        const auto mo = macros[i].toObject();
        patch.macros[size_t(i)].name = mo["name"].toString();
        for (const auto& tv : mo["targets"].toArray()) {
            const auto to = tv.toObject();
            patch.macros[size_t(i)].targets.append({ to["module"].toString(), to["param"].toString(),
                                                     float(to["min"].toDouble(0.0)),
                                                     float(to["max"].toDouble(1.0)) });
        }
    }

    const auto errors = patch.validate();
    if (!errors.isEmpty()) { *error = errors.first(); return std::nullopt; }
    return patch;
}

QJsonObject Patch::toJson() const
{
    QJsonArray mods;
    for (const auto& m : modules) {
        const auto* def = findModule(m.type.toStdString());
        QJsonObject params;
        if (def)
            for (auto it = m.params.begin(); it != m.params.end(); ++it) {
                const int pi = def->paramIndex(it.key().toStdString());
                if (pi >= 0) params[it.key()] = paramValueToJson(def->params[size_t(pi)], it.value());
            }
        QJsonObject mo{ { "id", m.id }, { "type", m.type },
                        { "section", m.section == Section::Global ? "global" : "voice" },
                        { "x", m.x }, { "y", m.y } };
        if (!params.isEmpty()) mo["params"] = params;
        mods.append(mo);
    }

    QJsonArray cables;
    for (const auto& c : this->cables)
        cables.append(QJsonObject{ { "from", c.from.toString() }, { "to", c.to.toString() } });

    QJsonArray macrosJson;
    for (const auto& mac : macros) {
        QJsonArray targets;
        for (const auto& t : mac.targets)
            targets.append(QJsonObject{ { "module", t.module }, { "param", t.param },
                                        { "min", t.min }, { "max", t.max } });
        macrosJson.append(QJsonObject{ { "name", mac.name }, { "targets", targets } });
    }

    return { { "modules", mods }, { "cables", cables }, { "macros", macrosJson } };
}

// ── Validation ─────────────────────────────────────────────────────────────

QStringList Patch::validate() const
{
    QStringList errors;
    QSet<QString> ids;

    for (const auto& m : modules) {
        if (m.id.isEmpty() || m.id.contains('.'))
            errors << QString("Module ids must be non-empty and contain no '.' (got '%1').").arg(m.id);
        else if (ids.contains(m.id))
            errors << QString("Two modules share the id '%1'; ids must be unique.").arg(m.id);
        ids.insert(m.id);

        const auto* def = findModule(m.type.toStdString());
        if (!def) { errors << QString("Unknown module type '%1'.").arg(m.type); continue; }
        if (!def->allowedIn(m.section))
            errors << QString("'%1' (%2) can only go in the %3 section.")
                          .arg(m.id, m.type, q(sectionName(def->section)));
    }

    for (const auto& c : cables) {
        const auto* src = find(c.from.module);
        const auto* dst = find(c.to.module);
        if (!src || !dst) {
            errors << QString("Cable %1 -> %2 refers to a module that doesn't exist.")
                          .arg(c.from.toString(), c.to.toString());
            continue;
        }
        const auto* sdef = findModule(src->type.toStdString());
        const auto* ddef = findModule(dst->type.toStdString());
        if (!sdef || !ddef) continue;
        if (sdef->outputIndex(c.from.port.toStdString()) < 0)
            errors << QString("'%1' (%2) has no output '%3'. Outputs: %4.")
                          .arg(src->id, src->type, c.from.port, listNames(sdef->outputs));
        if (ddef->inputIndex(c.to.port.toStdString()) < 0)
            errors << QString("'%1' (%2) has no input '%3'. Inputs: %4.")
                          .arg(dst->id, dst->type, c.to.port, listNames(ddef->inputs));
        if (src->section == Section::Voice && dst->section == Section::Global)
            errors << QString("Cable %1 -> %2 goes from a voice to the global section. "
                              "Send voices to global through voice_out -> voices instead.")
                          .arg(c.from.toString(), c.to.toString());
    }

    for (int i = 0; i < kNumMacros; ++i)
        for (const auto& t : macros[size_t(i)].targets) {
            const auto* m = find(t.module);
            const auto* def = m ? findModule(m->type.toStdString()) : nullptr;
            if (!def || def->paramIndex(t.param.toStdString()) < 0)
                errors << QString("Macro %1 targets %2.%3, which doesn't exist.")
                              .arg(i + 1).arg(t.module, t.param);
        }

    return errors;
}

QStringList Patch::warnings() const
{
    QStringList warnings;
    auto hasType = [&](const char* type, Section s) {
        return std::any_of(modules.begin(), modules.end(), [&](const PatchModule& m) {
            return m.type == type && m.section == s;
        });
    };
    auto isConnected = [&](const QString& id, const QString& port) {
        return std::any_of(cables.begin(), cables.end(),
                           [&](const Cable& c) { return c.to.module == id && c.to.port == port; });
    };

    bool outputFed = false;
    for (const auto& m : modules)
        if (m.type == "output" && isConnected(m.id, "left")) outputFed = true;
    if (!hasType("output", Section::Global))
        warnings << "No 'output' module: the patch is silent.";
    else if (!outputFed)
        warnings << "The output's left input has no cable: the patch is silent.";

    const bool anyVoice = std::any_of(modules.begin(), modules.end(),
                                      [](const PatchModule& m) { return m.section == Section::Voice; });
    if (anyVoice && !hasType("voice_out", Section::Voice))
        warnings << "Voice modules exist but there is no 'voice_out': voices are not heard.";
    if (hasType("voice_out", Section::Voice) && !hasType("voices", Section::Global))
        warnings << "There is a 'voice_out' but no global 'voices' module to collect it.";
    return warnings;
}

// ── Init patch ─────────────────────────────────────────────────────────────

Patch Patch::initPatch()
{
    QString error;
    auto patch = fromJson(QJsonObject{
        { "modules", QJsonArray{
            QJsonObject{ { "id", "voice_in" }, { "type", "voice_in" }, { "x", 40 }, { "y", 60 } },
            QJsonObject{ { "id", "vco1" }, { "type", "vco" }, { "x", 240 }, { "y", 40 } },
            QJsonObject{ { "id", "adsr1" }, { "type", "adsr" }, { "x", 240 }, { "y", 260 } },
            QJsonObject{ { "id", "vcf1" }, { "type", "vcf" }, { "x", 440 }, { "y", 40 },
                         { "params", QJsonObject{ { "cutoff", 900 }, { "cv_depth", 3 } } } },
            QJsonObject{ { "id", "vca1" }, { "type", "vca" }, { "x", 640 }, { "y", 40 } },
            QJsonObject{ { "id", "voice_out" }, { "type", "voice_out" }, { "x", 840 }, { "y", 60 } },
            QJsonObject{ { "id", "voices" }, { "type", "voices" }, { "section", "global" },
                         { "x", 40 }, { "y", 460 } },
            QJsonObject{ { "id", "output" }, { "type", "output" }, { "x", 240 }, { "y", 460 } },
        } },
        { "cables", QJsonArray{
            QJsonObject{ { "from", "voice_in.pitch" }, { "to", "vco1.pitch" } },
            QJsonObject{ { "from", "voice_in.gate" }, { "to", "adsr1.gate" } },
            QJsonObject{ { "from", "vco1.out" }, { "to", "vcf1.in" } },
            QJsonObject{ { "from", "adsr1.out" }, { "to", "vcf1.cutoff" } },
            QJsonObject{ { "from", "vcf1.out" }, { "to", "vca1.in" } },
            QJsonObject{ { "from", "adsr1.out" }, { "to", "vca1.cv" } },
            QJsonObject{ { "from", "vca1.out" }, { "to", "voice_out.left" } },
            QJsonObject{ { "from", "voices.left" }, { "to", "output.left" } },
            QJsonObject{ { "from", "voices.right" }, { "to", "output.right" } },
        } },
    }, &error);
    return patch ? *patch : Patch{};
}

// ── Edit operations ────────────────────────────────────────────────────────

namespace {

QString opAddModule(Patch& p, const QJsonObject& op)
{
    const QString id = op["id"].toString().trimmed();
    const QString type = op["type"].toString().trimmed();
    const auto* def = findModule(type.toStdString());
    if (!def) return QString("Unknown module type '%1'. Available: %2.").arg(type, listTypes());
    if (id.isEmpty()) return "add_module needs an \"id\" (e.g. \"vco2\").";
    if (p.find(id)) return QString("A module called '%1' already exists; pick another id.").arg(id);

    PatchModule m;
    m.id = id;
    m.type = type;
    if (op.contains("section")) {
        auto s = parseSection(op["section"].toString());
        if (!s) return "section must be 'voice' or 'global'.";
        m.section = *s;
    } else {
        m.section = defaultSection(*def);
    }
    // Place new modules after the existing ones unless a position is given.
    float maxX = 0.0f;
    for (const auto& other : p.modules)
        if (other.section == m.section) maxX = std::max(maxX, other.x + 200.0f);
    m.x = float(op["x"].toDouble(maxX));
    m.y = float(op["y"].toDouble(m.section == Section::Global ? 460.0 : 40.0));

    const auto params = op["params"].toObject();
    for (auto it = params.begin(); it != params.end(); ++it) {
        const int pi = def->paramIndex(it.key().toStdString());
        if (pi < 0)
            return QString("'%1' has no parameter '%2'. Parameters: %3.")
                .arg(type, it.key(), listParams(*def));
        QString err;
        auto v = parseParamValue(def->params[size_t(pi)], it.value(), &err);
        if (!v) return err;
        m.params[it.key()] = *v;
    }
    p.modules.append(m);
    return {};
}

QString opRemoveModule(Patch& p, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (!p.find(id)) return QString("No module called '%1'.").arg(id);
    p.modules.erase(std::remove_if(p.modules.begin(), p.modules.end(),
                                   [&](const PatchModule& m) { return m.id == id; }),
                    p.modules.end());
    p.cables.erase(std::remove_if(p.cables.begin(), p.cables.end(), [&](const Cable& c) {
                       return c.from.module == id || c.to.module == id;
                   }),
                   p.cables.end());
    for (auto& mac : p.macros)
        mac.targets.erase(std::remove_if(mac.targets.begin(), mac.targets.end(),
                                         [&](const MacroTarget& t) { return t.module == id; }),
                          mac.targets.end());
    return {};
}

QString opConnect(Patch& p, const QJsonObject& op, bool connect)
{
    auto from = parsePortRef(op["from"].toString());
    auto to = parsePortRef(op["to"].toString());
    if (!to) return "Needs \"to\" as \"module.port\", e.g. \"vcf1.in\".";

    if (connect) {
        if (!from) return "connect needs \"from\" as \"module.port\", e.g. \"vco1.out\".";
        const Cable cable{ *from, *to };
        if (!p.cables.contains(cable)) p.cables.append(cable);
        return {};
    }

    // disconnect: a specific cable, or every cable into "to" when "from" is omitted.
    const auto before = p.cables.size();
    p.cables.erase(std::remove_if(p.cables.begin(), p.cables.end(), [&](const Cable& c) {
                       return c.to == *to && (!from || c.from == *from);
                   }),
                   p.cables.end());
    if (p.cables.size() == before)
        return QString("No cable into %1%2 to remove.")
            .arg(to->toString(), from ? " from " + from->toString() : QString());
    return {};
}

QString opSetParam(Patch& p, const QJsonObject& op)
{
    auto* m = p.find(op["module"].toString());
    if (!m) return QString("No module called '%1'.").arg(op["module"].toString());
    const auto* def = findModule(m->type.toStdString());
    const QString name = op["param"].toString();
    const int pi = def->paramIndex(name.toStdString());
    if (pi < 0)
        return QString("'%1' (%2) has no parameter '%3'. Parameters: %4.")
            .arg(m->id, m->type, name, listParams(*def));
    QString err;
    auto v = parseParamValue(def->params[size_t(pi)], op["value"], &err);
    if (!v) return err;
    m->params[name] = *v;
    return {};
}

QString opSetMacro(Patch& p, const QJsonObject& op)
{
    const int index = op["index"].toInt(-1);
    if (index < 0 || index >= kNumMacros)
        return QString("Macro index must be 0 to %1.").arg(kNumMacros - 1);
    Macro macro;
    macro.name = op["name"].toString();
    for (const auto& tv : op["targets"].toArray()) {
        const auto t = tv.toObject();
        macro.targets.append({ t["module"].toString(), t["param"].toString(),
                               float(t["min"].toDouble(0.0)), float(t["max"].toDouble(1.0)) });
    }
    p.macros[size_t(index)] = macro;
    return {};
}

QString opMoveModule(Patch& p, const QJsonObject& op)
{
    auto* m = p.find(op["id"].toString());
    if (!m) return QString("No module called '%1'.").arg(op["id"].toString());
    m->x = float(op["x"].toDouble(m->x));
    m->y = float(op["y"].toDouble(m->y));
    return {};
}

} // namespace

QString applyOps(Patch& patch, const QJsonArray& ops)
{
    Patch work = patch;
    for (int i = 0; i < ops.size(); ++i) {
        const auto op = ops[i].toObject();
        const QString name = op["op"].toString();
        QString err;
        if (name == "add_module")         err = opAddModule(work, op);
        else if (name == "remove_module") err = opRemoveModule(work, op);
        else if (name == "connect")       err = opConnect(work, op, true);
        else if (name == "disconnect")    err = opConnect(work, op, false);
        else if (name == "set_param")     err = opSetParam(work, op);
        else if (name == "set_macro")     err = opSetMacro(work, op);
        else if (name == "move_module")   err = opMoveModule(work, op);
        else err = QString("Unknown op '%1'. Use add_module, remove_module, connect, "
                           "disconnect, set_param, set_macro or move_module.").arg(name);
        if (!err.isEmpty())
            return QString("Op %1 (%2): %3 No changes were made.").arg(i + 1).arg(name, err);
    }

    const auto errors = work.validate();
    if (!errors.isEmpty())
        return errors.first() + " No changes were made.";
    patch = work;
    return {};
}

// ── Catalog ────────────────────────────────────────────────────────────────

QJsonArray catalogJson()
{
    QJsonArray out;
    for (const auto& d : allModules()) {
        auto ports = [](const std::vector<PortDef>& list) {
            QJsonArray arr;
            for (const auto& p : list) {
                QJsonObject o{ { "name", q(p.name) }, { "kind", portKindName(p.kind) } };
                if (!p.description.empty()) o["description"] = q(p.description);
                arr.append(o);
            }
            return arr;
        };
        QJsonArray params;
        for (const auto& p : d.params) {
            QJsonObject o{ { "name", q(p.name) }, { "default", paramValueToJson(p, p.defaultValue) } };
            if (p.choices.empty()) {
                o["min"] = p.min;
                o["max"] = p.max;
                if (!p.unit.empty()) o["unit"] = q(p.unit);
            } else {
                QJsonArray choices;
                for (const auto& c : p.choices) choices.append(q(c));
                o["choices"] = choices;
            }
            if (!p.description.empty()) o["description"] = q(p.description);
            params.append(o);
        }
        out.append(QJsonObject{ { "type", q(d.type) }, { "section", sectionName(d.section) },
                                { "description", q(d.description) },
                                { "inputs", ports(d.inputs) }, { "outputs", ports(d.outputs) },
                                { "params", params } });
    }
    return out;
}

} // namespace OpenDaw::Modular
