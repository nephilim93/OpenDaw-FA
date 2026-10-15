#pragma once

#include "ModuleRegistry.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <optional>

namespace OpenDaw::Modular {

struct PatchModule {
    QString id;
    QString type;
    Section section = Section::Voice;   // Voice or Global (never Both)
    QMap<QString, float> params;        // only values that differ from defaults
    float x = 0.0f, y = 0.0f;           // editor position
};

struct PortRef {
    QString module, port;
    QString toString() const { return module + "." + port; }
    bool operator==(const PortRef& o) const { return module == o.module && port == o.port; }
};

struct Cable {
    PortRef from, to;
    bool operator==(const Cable& o) const { return from == o.from && to == o.to; }
};

struct MacroTarget {
    QString module, param;
    float min = 0.0f, max = 1.0f;
};

struct Macro {
    QString name;
    QVector<MacroTarget> targets;
};

constexpr int kNumMacros = 8;

// A modular patch: modules, cables and macros. Lives as JSON in the host
// plugin's state; every edit (AI or editor) goes through applyOps().
struct Patch {
    QVector<PatchModule> modules;
    QVector<Cable> cables;
    std::array<Macro, kNumMacros> macros;

    static std::optional<Patch> fromJson(const QJsonObject& json, QString* error);
    QJsonObject toJson() const;

    // Errors make the patch unusable; warnings explain why it may be silent.
    QStringList validate() const;
    QStringList warnings() const;

    const PatchModule* find(const QString& id) const;
    PatchModule* find(const QString& id);
    float paramValue(const PatchModule& m, int paramIndex) const;  // falls back to default

    // A basic subtractive voice: VCO -> VCF -> VCA, shaped by an ADSR.
    static Patch initPatch();
};

// Parses "module.port".
std::optional<PortRef> parsePortRef(const QString& text);

// Applies edit operations (add_module, remove_module, connect, disconnect,
// set_param, set_macro, move_module). All-or-nothing: on any error the
// patch is left unchanged and the error (with a fix hint) is returned.
QString applyOps(Patch& patch, const QJsonArray& ops);

// The module catalog as JSON, for the AI.
QJsonArray catalogJson();

} // namespace OpenDaw::Modular
