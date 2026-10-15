#include "AiToolDefs.h"
#include "AiToolExecutor.h"
#include "engine/EditManager.h"
#include "engine/modular/ModularSynthPlugin.h"
#include "modular/CompiledPatch.h"
#include <QJsonDocument>

// AI tools for OpenDaw's Modular Synth: list the module catalog, create the
// instrument, and read, replace or edit its patch. Patch logic lives in
// src/modular/Patch.* (validation, all-or-nothing edits); these handlers
// only resolve the track, apply the change as one undo step, and report.

namespace OpenDaw {

namespace {

QJsonObject propOf(const QString& type, const QString& description)
{
    return QJsonObject{ { "type", type }, { "description", description } };
}

QJsonObject trackRef()
{
    return QJsonObject{
        { "description", "Track name (string) or zero-based index (integer)." },
        { "oneOf", QJsonArray{ QJsonObject{ { "type", "string" } },
                               QJsonObject{ { "type", "integer" } } } },
    };
}

QJsonObject tool(const QString& name, const QString& description,
                 const QJsonObject& properties, const QJsonArray& required = {})
{
    QJsonObject schema{ { "type", "object" }, { "properties", properties } };
    if (!required.isEmpty()) schema["required"] = required;
    return QJsonObject{ { "name", name }, { "description", description },
                        { "input_schema", schema } };
}

const char* kConventions =
    "Signals: pitch is 1 unit per octave with 0 = C4 (MIDI 60), so semitones/12; gates are "
    "0 or 1; bipolar CV is -1..1, unipolar 0..1; audio about +/-1. "
    "Sections: 'voice' modules run once per held note (8-voice polyphony) and start from "
    "voice_in; their sound reaches the global section only through voice_out -> voices. "
    "'global' modules run all the time (clocks, sequencers, effects, generative patches) and "
    "end at output. Global outputs may feed voice inputs (broadcast to every voice); voice "
    "outputs may not feed global inputs directly. Multiple cables into one input are summed. "
    "Feedback loops are allowed (they add 16 samples of delay). Ports are written "
    "\"module_id.port\".";

// A compact description of a patch's state for tool results.
QJsonObject summarize(const Modular::Patch& patch)
{
    int voice = 0, global = 0;
    for (const auto& m : patch.modules)
        (m.section == Modular::Section::Global ? global : voice)++;

    QJsonArray macros;
    for (int i = 0; i < Modular::kNumMacros; ++i)
        if (!patch.macros[size_t(i)].targets.isEmpty())
            macros.append(QJsonObject{ { "index", i }, { "name", patch.macros[size_t(i)].name },
                                       { "targets", patch.macros[size_t(i)].targets.size() } });

    QJsonObject out{ { "voice_modules", voice }, { "global_modules", global },
                     { "cables", int(patch.cables.size()) } };
    if (auto compiled = Modular::CompiledPatch::build(patch, 48000.0))
        out["feedback_cables"] = compiled->numFeedbackCables();
    if (!macros.isEmpty()) out["macros"] = macros;
    const auto warnings = patch.warnings();
    if (!warnings.isEmpty()) out["warnings"] = QJsonArray::fromStringList(warnings);
    return out;
}

QJsonObject patchSchema(const QString& description)
{
    return QJsonObject{ { "type", "object" }, { "description", description } };
}

// A code module's ports and parameters as compiled, so the AI can wire it.
QJsonObject describeCodeModule(const Modular::PatchModule& m)
{
    QJsonObject out{ { "id", m.id }, { "section", m.section == Modular::Section::Global ? "global" : "voice" } };
    const auto* def = Modular::moduleDef(m);
    if (!def) return out;
    QJsonArray inputs, outputs, params;
    for (const auto& p : def->inputs) inputs.append(QString::fromStdString(p.name));
    for (const auto& p : def->outputs) outputs.append(QString::fromStdString(p.name));
    for (const auto& p : def->params) {
        QJsonObject o{ { "name", QString::fromStdString(p.name) }, { "min", p.min },
                       { "max", p.max }, { "default", p.defaultValue } };
        if (!p.unit.empty()) o["unit"] = QString::fromStdString(p.unit);
        params.append(o);
    }
    out["inputs"] = inputs;
    out["outputs"] = outputs;
    out["params"] = params;
    return out;
}

const char* kCodeModuleHelp =
    "Write Faust (https://faust.grame.fr) with a 'process = ...;' definition; "
    "import(\"stdfaust.lib\") gives os., fi., ef., re., en., ba. and the rest. Its audio "
    "inputs and outputs become ports: one input is 'in', several are 'in1'..'inN' (outputs "
    "likewise 'out' or 'out1'..'outN'). Each hslider/vslider/nentry/button/checkbox becomes a "
    "parameter named by its label, with its range and default; add [unit:Hz] to a label to "
    "show a unit. Signals follow the Modular Synth conventions (pitch in octaves, 0 = C4), so "
    "convert in the code, e.g. 261.626 * 2^pitch for Hz. ffunction, fconstant, fvariable and "
    "soundfile are not allowed; at most 8 code modules per patch and 64 KB of code each.";

} // namespace

QJsonArray AiToolDefs::modularTools()
{
    QJsonArray tools;

    tools.append(tool("list_modular_modules",
        QString("List every Modular Synth module: its section, inputs and outputs (with signal "
                "kind), and parameters (ranges, units, choices, defaults). Call this before "
                "building or editing a patch if you are unsure of a module's ports or "
                "parameters. ") + kConventions,
        QJsonObject{}));

    tools.append(tool("create_modular_synth",
        QString("Load OpenDaw's Modular Synth as a track's instrument (replacing its current "
                "instrument; MIDI FX are kept). Without 'patch' it starts from the init patch "
                "(voice_in -> vco -> vcf -> vca -> voice_out, shaped by an adsr). Pass 'patch' "
                "to build the whole sound in one step. Undoable.\n") + kConventions,
        QJsonObject{
            { "track", trackRef() },
            { "patch", patchSchema(
                "Optional full patch: {\"modules\": [{\"id\", \"type\", \"section\" "
                "(\"voice\"|\"global\", optional), \"params\": {name: value or choice name}}], "
                "\"cables\": [{\"from\": \"id.port\", \"to\": \"id.port\"}], \"macros\": "
                "[{\"name\", \"targets\": [{\"module\", \"param\", \"min\", \"max\"}]}]}. "
                "Code modules have type \"faust\" and their source in \"code\".") },
        },
        QJsonArray{ "track" }));

    tools.append(tool("get_modular_patch",
        "Read a track's Modular Synth patch as JSON, with a summary (module counts, cables, "
        "feedback loops, macros) and warnings such as a silent output.",
        QJsonObject{ { "track", trackRef() } }, QJsonArray{ "track" }));

    tools.append(tool("set_modular_patch",
        "Replace a track's whole Modular Synth patch (same format as create_modular_synth). "
        "Prefer edit_modular_patch for changes to an existing patch. The patch is validated "
        "first; on any error nothing changes. Undoable.",
        QJsonObject{ { "track", trackRef() },
                     { "patch", patchSchema("The full patch, as in create_modular_synth.") } },
        QJsonArray{ "track", "patch" }));

    QJsonObject opItem{
        { "type", "object" },
        { "properties", QJsonObject{
            { "op", QJsonObject{ { "type", "string" }, { "enum", QJsonArray{
                "add_module", "remove_module", "connect", "disconnect", "set_param",
                "set_macro", "move_module", "write_module" } } } },
            { "id", propOf("string", "add_module/remove_module/move_module/write_module: module id, e.g. \"vco2\".") },
            { "code", propOf("string", "write_module: Faust source (see write_modular_module).") },
            { "type", propOf("string", "add_module: module type from list_modular_modules.") },
            { "section", propOf("string", "add_module: \"voice\" or \"global\" (default: the module's usual section).") },
            { "params", propOf("object", "add_module: initial parameter values.") },
            { "from", propOf("string", "connect/disconnect: source \"module_id.output\".") },
            { "to", propOf("string", "connect/disconnect: destination \"module_id.input\". disconnect without 'from' removes every cable into this input.") },
            { "module", propOf("string", "set_param: module id.") },
            { "param", propOf("string", "set_param: parameter name.") },
            { "value", QJsonObject{ { "description", "set_param: number, or a choice name for choice parameters." },
                                    { "oneOf", QJsonArray{ QJsonObject{ { "type", "number" } },
                                                           QJsonObject{ { "type", "string" } } } } } },
            { "index", propOf("integer", "set_macro: macro slot 0-7.") },
            { "name", propOf("string", "set_macro: macro label, e.g. \"Brightness\".") },
            { "targets", propOf("array", "set_macro: [{\"module\", \"param\", \"min\", \"max\"}]; the macro sweeps each parameter from min to max.") },
            { "x", propOf("number", "add_module/move_module: editor position.") },
            { "y", propOf("number", "add_module/move_module: editor position.") },
        } },
        { "required", QJsonArray{ "op" } },
    };
    tools.append(tool("edit_modular_patch",
        "Change a track's Modular Synth patch with a list of operations, applied in order as "
        "one undoable step. All-or-nothing: if any operation fails, nothing changes and the "
        "error says how to fix it. Operations: add_module {id, type, section?, params?}, "
        "remove_module {id} (also removes its cables), connect {from, to}, disconnect {to, "
        "from?}, set_param {module, param, value}, set_macro {index, name, targets}, "
        "move_module {id, x, y}, write_module {id, code, section?, params?} (a Faust code "
        "module, as in write_modular_module). Batch related changes into one call.",
        QJsonObject{ { "track", trackRef() },
                     { "ops", QJsonObject{ { "type", "array" }, { "items", opItem } } } },
        QJsonArray{ "track", "ops" }));

    tools.append(tool("write_modular_module",
        QString("Add a code module to a track's Modular Synth patch, or replace the code of an "
                "existing one, for DSP the module library lacks (a special filter, waveshaper, "
                "physical model, ...). ") + kCodeModuleHelp
            + " The code compiles immediately: a compile error is returned with its line "
              "number and nothing changes. Replacing code keeps the cables and parameter values "
              "that still fit and reports the ones dropped. The result lists the module's ports "
              "and parameters; wire them with edit_modular_patch. Undoable.",
        QJsonObject{
            { "track", trackRef() },
            { "id", propOf("string", "Module id, e.g. \"fold1\". An existing code module with this id is updated.") },
            { "code", propOf("string", "The Faust source.") },
            { "section", propOf("string", "\"voice\" (default for a new module: one instance per note) or \"global\".") },
            { "params", propOf("object", "Optional parameter values by name, as in add_module.") },
        },
        QJsonArray{ "track", "id", "code" }));

    tools.append(tool("set_modular_macro",
        "Move one of the Modular Synth's 8 macro knobs (0..1). A macro sweeps the parameters "
        "assigned to it with set_macro. Macros are also automatable in the DAW.",
        QJsonObject{ { "track", trackRef() },
                     { "index", propOf("integer", "Macro slot 0-7.") },
                     { "value", QJsonObject{ { "type", "number" }, { "minimum", 0 }, { "maximum", 1 },
                                             { "description", "Knob position 0..1." } } } },
        QJsonArray{ "track", "index", "value" }));

    return tools;
}

void AiToolExecutor::registerModularHandlers()
{
    auto findSynth = [this](te::AudioTrack* track) -> ModularSynthPlugin* {
        return dynamic_cast<ModularSynthPlugin*>(editMgr_->getTrackInstrument(track));
    };
    auto noSynth = [this](const QString& id, te::AudioTrack* track) {
        return err(id, QString("Track '%1' has no Modular Synth. Call create_modular_synth first.")
                           .arg(QString::fromUtf8(track->getName().toRawUTF8())));
    };

    handlers_["list_modular_modules"] = [this](const QJsonObject&, const QString& id) {
        return ok(id, QJsonObject{ { "conventions", kConventions },
                                   { "modules", Modular::catalogJson() } });
    };

    handlers_["create_modular_synth"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");

        // Validate a given patch before touching the track.
        std::optional<Modular::Patch> patch;
        if (input.contains("patch")) {
            QString error;
            patch = Modular::Patch::fromJson(input["patch"].toObject(), &error);
            if (!patch) return err(id, error + " Nothing was changed.");
        }

        editMgr_->edit()->getUndoManager().beginNewTransaction("AI: Create Modular Synth");
        auto* synth = dynamic_cast<ModularSynthPlugin*>(editMgr_->setTrackModularSynth(*track));
        if (!synth) return err(id, "Could not create the Modular Synth.");
        if (patch) synth->setPatch(*patch, {});
        emitTracksChanged();

        QJsonObject out = summarize(synth->patch());
        out["created"] = true;
        if (!patch) out["patch"] = synth->patch().toJson();
        return ok(id, out);
    };

    handlers_["get_modular_patch"] = [this, findSynth, noSynth](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* synth = findSynth(track);
        if (!synth) return noSynth(id, track);
        QJsonObject out = summarize(synth->patch());
        out["patch"] = synth->patch().toJson();
        return ok(id, out);
    };

    handlers_["set_modular_patch"] = [this, findSynth, noSynth](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* synth = findSynth(track);
        if (!synth) return noSynth(id, track);

        QString error;
        auto patch = Modular::Patch::fromJson(input["patch"].toObject(), &error);
        if (!patch) return err(id, error + " Nothing was changed.");
        synth->setPatch(*patch, "AI: Replace Modular Patch");
        emitEditChanged();
        return ok(id, summarize(*patch));
    };

    handlers_["edit_modular_patch"] = [this, findSynth, noSynth](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* synth = findSynth(track);
        if (!synth) return noSynth(id, track);

        auto patch = synth->patch();
        const QString error = Modular::applyOps(patch, input["ops"].toArray());
        if (!error.isEmpty()) return err(id, error);
        synth->setPatch(patch, "AI: Edit Modular Patch");
        emitEditChanged();

        QJsonObject out = summarize(patch);
        out["applied"] = int(input["ops"].toArray().size());
        return ok(id, out);
    };

    handlers_["write_modular_module"] = [this, findSynth, noSynth](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* synth = findSynth(track);
        if (!synth) return noSynth(id, track);

        const QString moduleId = input["id"].toString().trimmed();
        QJsonObject op{ { "op", "write_module" }, { "id", moduleId }, { "code", input["code"] } };
        for (const char* key : { "section", "params" })
            if (input.contains(key)) op[key] = input[key];

        const auto before = synth->patch();
        auto patch = before;
        const QString error = Modular::applyOps(patch, QJsonArray{ op });
        if (!error.isEmpty()) return err(id, error);
        synth->setPatch(patch, "AI: Write Modular Code Module");
        emitEditChanged();

        QJsonObject out = summarize(patch);
        out["created"] = before.find(moduleId) == nullptr;
        if (const auto* m = patch.find(moduleId)) out["module"] = describeCodeModule(*m);
        QJsonArray dropped;
        for (const auto& c : before.cables)
            if (!patch.cables.contains(c))
                dropped.append(c.from.toString() + " -> " + c.to.toString());
        if (!dropped.isEmpty()) out["removed_cables"] = dropped;
        int droppedTargets = 0;
        for (int i = 0; i < Modular::kNumMacros; ++i)
            droppedTargets += int(before.macros[size_t(i)].targets.size() - patch.macros[size_t(i)].targets.size());
        if (droppedTargets > 0) out["removed_macro_targets"] = droppedTargets;
        return ok(id, out);
    };

    handlers_["set_modular_macro"] = [this, findSynth, noSynth](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* synth = findSynth(track);
        if (!synth) return noSynth(id, track);

        const int index = input["index"].toInt(-1);
        auto* macro = synth->macro(index);
        if (!macro) return err(id, QString("Macro index must be 0-%1.").arg(Modular::kNumMacros - 1));
        const float value = float(std::clamp(input["value"].toDouble(0.0), 0.0, 1.0));

        editMgr_->edit()->getUndoManager().beginNewTransaction("AI: Set Modular Macro");
        macro->setParameter(value, juce::sendNotification);
        emitEditChanged();

        const auto m = synth->patch().macros[size_t(index)];  // patch() returns a copy
        QJsonObject out{ { "index", index }, { "value", value }, { "name", m.name } };
        if (m.targets.isEmpty())
            out["warning"] = "This macro has no targets yet; assign some with set_macro.";
        return ok(id, out);
    };
}

} // namespace OpenDaw
