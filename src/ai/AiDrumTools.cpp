#include "AiToolDefs.h"
#include "AiToolExecutor.h"
#include "drums/DrumKit.h"
#include "drums/KitPresets.h"
#include "drums/SampleLibrary.h"
#include "engine/EditManager.h"
#include "engine/drums/DrumMachinePlugin.h"
#include "modular/Patch.h"
#include "music/DrumPatterns.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <map>

// AI tools for OpenDaw's Drum Machine: create the instrument, read and edit
// its kit, list the groove styles, and write a groove from the style library
// into a MIDI clip. Kit logic lives in src/drums, grooves in
// src/music/DrumPatterns; these handlers resolve the track, apply each
// change as one undo step, and report.

namespace OpenDaw {

namespace {

QJsonObject propOf(const QString& type, const QString& description)
{
    return QJsonObject{ { "type", type }, { "description", description } };
}

QJsonObject numberOf(const QString& description, double min, double max)
{
    return QJsonObject{ { "type", "number" }, { "description", description },
                        { "minimum", min }, { "maximum", max } };
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

// General MIDI percussion names, for pads and results.
const std::map<int, const char*>& gmNames()
{
    static const std::map<int, const char*> names = {
        { 35, "Acoustic Bass Drum" }, { 36, "Kick" }, { 37, "Side Stick" }, { 38, "Snare" },
        { 39, "Clap" }, { 40, "Electric Snare" }, { 41, "Low Floor Tom" }, { 42, "Closed Hat" },
        { 43, "High Floor Tom" }, { 44, "Pedal Hat" }, { 45, "Low Tom" }, { 46, "Open Hat" },
        { 47, "Low-Mid Tom" }, { 48, "High-Mid Tom" }, { 49, "Crash" }, { 50, "High Tom" },
        { 51, "Ride" }, { 52, "China" }, { 53, "Ride Bell" }, { 54, "Tambourine" },
        { 55, "Splash" }, { 56, "Cowbell" }, { 57, "Crash 2" }, { 59, "Ride 2" },
        { 60, "High Bongo" }, { 61, "Low Bongo" }, { 62, "Mute Conga" }, { 63, "Open Conga" },
        { 64, "Low Conga" }, { 65, "High Timbale" }, { 66, "Low Timbale" }, { 69, "Cabasa" },
        { 70, "Maracas" }, { 75, "Claves" }, { 76, "High Wood Block" }, { 77, "Low Wood Block" },
    };
    return names;
}

QString gmName(int note)
{
    const auto& names = gmNames();
    auto it = names.find(note);
    return it != names.end() ? QString(it->second) : QString("Note %1").arg(note);
}

// One line per pad, for tool results.
QJsonArray padSummary(const Drums::Kit& kit)
{
    QJsonArray pads;
    for (const auto& p : kit.pads) {
        QString sound = p.source == Drums::PadSource::Synth ? p.module : "sample " + QFileInfo(p.sample).fileName();
        pads.append(QString("%1 %2: %3").arg(p.note).arg(p.name, sound));
    }
    return pads;
}

// The synth pad modules with their parameters, from the modular catalog.
QJsonArray synthModulesJson()
{
    const QStringList types = Drums::synthPadModules();
    QJsonArray out;
    for (const auto& entry : Modular::catalogJson()) {
        const auto o = entry.toObject();
        if (!types.contains(o["type"].toString())) continue;
        out.append(QJsonObject{ { "type", o["type"] }, { "description", o["description"] },
                                { "params", o["params"] } });
    }
    return out;
}

} // namespace

QJsonArray AiToolDefs::drumTools()
{
    QJsonArray tools;

    tools.append(tool("list_drum_styles",
        "List the groove styles write_drum_pattern knows (rock, pop, blues_shuffle, funk, "
        "hiphop, trap, house, ...), each with its feel, time signature, default swing and "
        "typical tempo range.",
        QJsonObject{}));

    tools.append(tool("create_drum_machine",
        "Load OpenDaw's Drum Machine as a track's instrument (replacing any other). It plays "
        "General MIDI drum notes (36 kick, 38 snare, 42/44/46 hats, 41-50 toms, 49/57 crash, "
        "51 ride...) with a ready-made kit of synth drums. Use it for every drum track. "
        "write_drum_pattern loads it by itself on a track without an instrument.",
        QJsonObject{ { "track", trackRef() } },
        QJsonArray{ "track" }));

    tools.append(tool("get_drum_kit",
        "Read a Drum Machine's kit: each pad's note, name, sound (a synth voice and its "
        "parameters, or a sample file), level, pan, tune and choke group, plus the synth "
        "voices available for pads and their parameters.",
        QJsonObject{ { "track", trackRef() } },
        QJsonArray{ "track" }));

    tools.append(tool("set_drum_pad",
        "Change, add or remove one pad of a Drum Machine's kit, as one undoable step. Only the "
        "fields you give change. Synth pads use a drum voice (kick, snare, hihat, clap, tom, "
        "cymbal, rimshot, cowbell, ...) with optional parameter values (see get_drum_kit); "
        "sample pads play an audio file (wav, aiff, flac, ogg): a path relative to the user's "
        "samples folder (preferred, so the kit travels; see get_drum_kit) or an absolute one.",
        QJsonObject{
            { "track", trackRef() },
            { "note", numberOf("The pad's MIDI note (GM: 36 kick, 38 snare, 42 closed hat...).", 0, 127) },
            { "name", propOf("string", "Pad name; defaults to the GM name for a new pad.") },
            { "source", propOf("string", "\"synth\" or \"sample\".") },
            { "module", propOf("string", "Synth pads: the drum voice, e.g. \"tom\". Changing it resets its params.") },
            { "params", propOf("object", "Synth pads: parameter values by name, e.g. {\"tune\": 160, \"decay\": 0.3}; merged with the current ones.") },
            { "sample", propOf("string", "Sample pads: the audio file, relative to the samples folder (e.g. \"808/kick.wav\") or absolute.") },
            { "level", numberOf("Linear gain, 0-1.5 (default 0.8).", 0, 1.5) },
            { "pan", numberOf("-1 left .. 1 right.", -1, 1) },
            { "tune", numberOf("Semitones, -24..24.", -24, 24) },
            { "choke", numberOf("Choke group 1-16 (pads in a group cut each other off, like hats); 0 = none.", 0, 16) },
            { "remove", propOf("boolean", "Remove this pad instead.") },
        },
        QJsonArray{ "track", "note" }));

    tools.append(tool("list_drum_kits",
        "List the Drum Machine's kit presets: built-in synth kits (Synth Kit, Synth 808, Synth 909) "
        "and the user's own, saved in the Kits folder of their samples folder.",
        QJsonObject{}, QJsonArray{}));

    tools.append(tool("load_drum_kit",
        "Replace a Drum Machine's whole kit with a preset (see list_drum_kits), as one undoable "
        "step. Loads a Drum Machine on a track without an instrument.",
        QJsonObject{
            { "track", trackRef() },
            { "name", propOf("string", "The preset's name (case doesn't matter).") },
        },
        QJsonArray{ "track", "name" }));

    tools.append(tool("save_drum_kit",
        "Save a Drum Machine's current kit as a user preset in the Kits folder, so it can be loaded "
        "on other tracks and in other projects. Samples inside the samples folder are stored "
        "relative to it, so the preset travels with the folder.",
        QJsonObject{
            { "track", trackRef() },
            { "name", propOf("string", "Preset name (no / \\ : * ? \" < > |).") },
            { "overwrite", propOf("boolean", "Replace a user preset of that name (default false).") },
        },
        QJsonArray{ "track", "name" }));

    tools.append(tool("write_drum_pattern",
        "Write an idiomatic drum groove from the style library into a MIDI clip (GM drum notes "
        "on channel 10), as one undoable step: kick, snare, hats/ride, ghost notes, fills every "
        "N bars ending in a crash, swing and humanize. Prefer this over add_midi_notes for "
        "drums; adjust the result with the MIDI note tools if needed. Creates a clip unless "
        "clip_index is given, and loads a Drum Machine on a track without an instrument. Call "
        "it once per section (e.g. verse intensity 0.4, chorus 0.8, a different variation for "
        "the bridge).",
        QJsonObject{
            { "track", trackRef() },
            { "style", propOf("string", "Groove style; see list_drum_styles (e.g. rock, blues_shuffle, funk, house).") },
            { "bars", numberOf("Length in bars (default 4).", 1, 256) },
            { "start_beat", numberOf("Where it starts: a song beat for a new clip, or a beat inside the clip with clip_index (default 0).", 0, 100000) },
            { "clip_index", numberOf("Write into this existing MIDI clip instead of creating one.", 0, 10000) },
            { "replace", propOf("boolean", "With clip_index: first remove the clip's notes in the written range (default true).") },
            { "intensity", numberOf("0 sparse .. 1 busy (more hats, ghost notes, kicks). Default 0.5.", 0, 1) },
            { "swing", numberOf("0-100, 100 = triplet feel. Omit for the style's default.", 0, 100) },
            { "fill_every", numberOf("A fill ends every Nth bar and the last bar; 0 = no fills. Default 4.", 0, 64) },
            { "fill_beats", numberOf("Fill length in beats (default 1).", 0.25, 8) },
            { "crash_on_first", propOf("boolean", "Crash on the first downbeat (default true).") },
            { "humanize", numberOf("0-1 timing (up to 10 ms) and velocity looseness. Default 0.", 0, 1) },
            { "variation", numberOf("Which variant of the style (default 0); try 1 or 2 for a B section.", 0, 16) },
            { "seed", numberOf("Varies fills and humanize (default 1).", 0, 1000000) },
            { "name", propOf("string", "Name for a new clip.") },
        },
        QJsonArray{ "track", "style" }));

    return tools;
}

void AiToolExecutor::registerDrumHandlers()
{
    auto findDrums = [this](te::AudioTrack* track) -> DrumMachinePlugin* {
        return dynamic_cast<DrumMachinePlugin*>(editMgr_->getTrackInstrument(track));
    };
    auto trackName = [](te::AudioTrack* track) { return QString::fromUtf8(track->getName().toRawUTF8()); };
    auto noDrums = [this, trackName](const QString& id, te::AudioTrack* track) {
        return err(id, QString("Track '%1' has no Drum Machine. Call create_drum_machine first.")
                           .arg(trackName(track)));
    };

    handlers_["list_drum_styles"] = [this](const QJsonObject&, const QString& id) {
        QJsonArray styles;
        for (const auto& s : DrumPatterns::styles())
            styles.append(QJsonObject{ { "name", s.name }, { "description", s.description },
                                       { "beats_per_bar", s.beatsPerBar },
                                       { "default_swing", s.defaultSwing },
                                       { "tempo", QString("%1-%2 BPM").arg(s.tempoMin).arg(s.tempoMax) } });
        return ok(id, QJsonObject{ { "styles", styles } });
    };

    handlers_["list_drum_kits"] = [this](const QJsonObject&, const QString& id) {
        const QString folder = Drums::samplesFolder();
        QJsonArray presets;
        for (const auto& p : Drums::listKitPresets(folder))
            presets.append(QJsonObject{ { "name", p.name }, { "built_in", p.builtIn } });
        return ok(id, QJsonObject{ { "presets", presets }, { "kits_folder", Drums::kitsFolder(folder) } });
    };

    handlers_["load_drum_kit"] = [this, findDrums, noDrums](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        const QString folder = Drums::samplesFolder();
        QString error;
        const auto kit = Drums::loadKitPreset(input["name"].toString(), folder, &error);
        if (!kit) return err(id, error + " Nothing was changed.");
        auto* drums = findDrums(track);
        if (!drums && editMgr_->getTrackInstrument(track)) return noDrums(id, track);

        // One undo step loads the Drum Machine (if needed) and the kit.
        editMgr_->edit()->getUndoManager().beginNewTransaction("AI: Load Drum Kit");
        QJsonObject out;
        if (!drums) {
            drums = dynamic_cast<DrumMachinePlugin*>(editMgr_->setTrackDrumMachine(*track));
            if (!drums) return err(id, "Could not create the Drum Machine.");
            out["drum_machine_created"] = true;
            emitTracksChanged();
        }
        drums->setKit(*kit, {});
        emitEditChanged();
        out["loaded"] = Drums::findKitPreset(input["name"].toString(), folder)->name;
        out["pads"] = padSummary(drums->kit());
        if (!drums->warnings().isEmpty()) out["warnings"] = QJsonArray::fromStringList(drums->warnings());
        return ok(id, out);
    };

    handlers_["save_drum_kit"] = [this, findDrums, noDrums](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* drums = findDrums(track);
        if (!drums) return noDrums(id, track);
        const QString folder = Drums::samplesFolder();
        const QString name = input["name"].toString().trimmed();
        QString error;
        QStringList outside;
        if (!Drums::saveKitPreset(drums->kit(), name, folder, input["overwrite"].toBool(false), &error, &outside))
            return err(id, error);
        QJsonObject out{ { "saved", Drums::findKitPreset(name, folder)->name },
                         { "file", Drums::findKitPreset(name, folder)->file } };
        if (!outside.isEmpty())
            out["outside_samples_folder"] = QString("These samples are outside the samples folder, so the preset "
                                                    "only finds them on this computer: %1.").arg(outside.join(", "));
        return ok(id, out);
    };

    handlers_["create_drum_machine"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        editMgr_->edit()->getUndoManager().beginNewTransaction("AI: Create Drum Machine");
        auto* drums = dynamic_cast<DrumMachinePlugin*>(editMgr_->setTrackDrumMachine(*track));
        if (!drums) return err(id, "Could not create the Drum Machine.");
        emitTracksChanged();
        return ok(id, QJsonObject{ { "created", true }, { "pads", padSummary(drums->kit()) } });
    };

    handlers_["get_drum_kit"] = [this, findDrums, noDrums](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* drums = findDrums(track);
        if (!drums) return noDrums(id, track);
        QJsonObject out{ { "kit", drums->kit().toJson() }, { "synth_voices", synthModulesJson() },
                         { "samples_folder", Drums::samplesFolder() } };
        if (!drums->warnings().isEmpty()) out["warnings"] = QJsonArray::fromStringList(drums->warnings());
        return ok(id, out);
    };

    handlers_["set_drum_pad"] = [this, findDrums, noDrums](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        auto* drums = findDrums(track);
        if (!drums) return noDrums(id, track);
        const int note = input["note"].toInt(-1);
        if (note < 0 || note > 127) return err(id, "note must be a MIDI note 0-127.");

        // Edit the kit as JSON, then let Kit::fromJson validate the whole result.
        QJsonObject kitJson = drums->kit().toJson();
        QJsonArray pads = kitJson["pads"].toArray();
        int index = -1;
        for (int i = 0; i < pads.size(); ++i)
            if (pads[i].toObject()["note"].toInt() == note) index = i;

        QString action;
        if (input["remove"].toBool(false)) {
            if (index < 0) return err(id, QString("There is no pad on note %1.").arg(note));
            pads.removeAt(index);
            action = "removed";
        } else {
            QJsonObject pad = index >= 0 ? pads[index].toObject()
                                         : QJsonObject{ { "note", note }, { "name", gmName(note) },
                                                        { "source", "synth" }, { "level", 0.8 } };
            for (const char* key : { "name", "source", "sample", "level", "pan", "tune", "choke" })
                if (input.contains(key)) pad[key] = input[key];
            if (input.contains("module") && input["module"] != pad["module"]) {
                pad["module"] = input["module"];
                pad.remove("params");   // another voice has other parameters
            }
            if (input.contains("params")) {
                QJsonObject params = pad["params"].toObject();
                const auto changes = input["params"].toObject();
                for (auto it = changes.begin(); it != changes.end(); ++it) params[it.key()] = it.value();
                pad["params"] = params;
            }
            if (pad["source"].toString() == "sample") {
                const QString folder = Drums::samplesFolder();
                const QString path = pad["sample"].toString().trimmed();
                const QString file = Drums::resolveSamplePath(path, folder);
                if (!path.isEmpty() && !QFileInfo(file).isFile())
                    return err(id, QString("No such file: %1. Give a wav, aiff, flac or ogg file, relative "
                                           "to the samples folder (%2) or absolute. Nothing was changed.")
                                       .arg(file, folder));
                // Files inside the samples folder are kept relative, so the kit travels.
                if (!path.isEmpty()) pad["sample"] = Drums::toKitPath(file, folder);
            } else if (!pad.contains("module")) {
                return err(id, QString("A new synth pad needs a module. Use one of: %1.")
                                   .arg(Drums::synthPadModules().join(", ")));
            }
            if (index >= 0) pads[index] = pad;
            else pads.append(pad);
            action = index >= 0 ? "changed" : "added";
        }
        kitJson["pads"] = pads;

        QString error;
        const auto kit = Drums::Kit::fromJson(kitJson, &error);
        if (!kit) return err(id, error + " Nothing was changed.");
        drums->setKit(*kit, "AI: Set Drum Pad");
        emitEditChanged();

        QJsonObject out{ { action, note } };
        for (const auto& p : kit->toJson()["pads"].toArray())
            if (p.toObject()["note"].toInt() == note) out["pad"] = p.toObject();
        if (!drums->warnings().isEmpty()) out["warnings"] = QJsonArray::fromStringList(drums->warnings());
        return ok(id, out);
    };

    handlers_["write_drum_pattern"] = [this, findDrums, trackName](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");

        DrumPatterns::Spec spec;
        spec.style = input["style"].toString().trimmed();
        spec.bars = std::clamp(input["bars"].toInt(4), 1, 256);
        spec.intensity = std::clamp(input["intensity"].toDouble(0.5), 0.0, 1.0);
        spec.swing = input.contains("swing") ? std::clamp(input["swing"].toDouble(), 0.0, 100.0) : -1.0;
        spec.fillEvery = std::clamp(input["fill_every"].toInt(4), 0, 64);
        spec.fillBeats = std::clamp(input["fill_beats"].toDouble(1.0), 0.25, 8.0);
        spec.crashOnFirst = input["crash_on_first"].toBool(true);
        spec.humanize = std::clamp(input["humanize"].toDouble(0.0), 0.0, 1.0);
        spec.variation = std::clamp(input["variation"].toInt(0), 0, 16);
        spec.seed = input["seed"].toInt(1);
        spec.tempo = editMgr_->getBpm() > 0 ? editMgr_->getBpm() : 120.0;

        const auto groove = DrumPatterns::generate(spec);
        if (!groove.error.isEmpty()) return err(id, groove.error);
        if (groove.notes.isEmpty()) return err(id, "The groove produced no notes.");

        int beatsPerBar = 4;
        for (const auto& s : DrumPatterns::styles())
            if (s.name == spec.style) beatsPerBar = s.beatsPerBar;
        const double lengthBeats = double(spec.bars * beatsPerBar);
        const double startBeat = std::max(0.0, input["start_beat"].toDouble(0.0));
        constexpr int kDrumChannel = 10;

        auto* um = &editMgr_->edit()->getUndoManager();
        um->beginNewTransaction("AI: Write Drum Pattern");

        // A drum track without an instrument would be silent: give it a Drum Machine.
        QJsonObject out;
        auto* instrument = editMgr_->getTrackInstrument(track);
        if (!instrument) {
            if (!editMgr_->setTrackDrumMachine(*track))
                return err(id, "Could not create the Drum Machine.");
            out["drum_machine_created"] = true;
        } else if (!findDrums(track)) {
            out["note"] = QString("Track '%1' plays '%2', not the Drum Machine: GM drum notes "
                                  "may not sound right. Call create_drum_machine to switch.")
                              .arg(trackName(track), QString::fromUtf8(instrument->getName().toRawUTF8()));
        }

        te::MidiClip* midiClip = nullptr;
        double noteOffset = startBeat;
        if (input.contains("clip_index")) {
            const int clipIdx = input["clip_index"].toInt();
            auto clips = track->getClips();
            if (clipIdx < 0 || clipIdx >= clips.size())
                return err(id, QString("Clip index %1 out of range (track has %2 clips).")
                                   .arg(clipIdx).arg(clips.size()));
            midiClip = dynamic_cast<te::MidiClip*>(clips[clipIdx]);
            if (!midiClip) return err(id, QString("Clip at index %1 is not a MIDI clip.").arg(clipIdx));
            if (input["replace"].toBool(true)) {
                auto& seq = midiClip->getSequence();
                const auto existing = seq.getNotes();   // a copy: removing edits the list
                for (auto* n : existing) {
                    const double b = n->getStartBeat().inBeats();
                    if (b >= startBeat - 1e-9 && b < startBeat + lengthBeats - 1e-9) seq.removeNote(*n, um);
                }
            }
        } else {
            midiClip = editMgr_->addMidiClipToTrack(*track, startBeat, lengthBeats);
            if (!midiClip) return err(id, "Failed to create MIDI clip.");
            midiClip->setMidiChannel(te::MidiChannel(kDrumChannel));
            const QString name = input["name"].toString();
            midiClip->setName(juce::String((name.isEmpty() ? QString("%1 drums").arg(spec.style) : name).toStdString()));
            if (!editMgr_->isMidiTrack(track)) editMgr_->markAsMidiTrack(track);
            noteOffset = 0.0;
        }

        auto& seq = midiClip->getSequence();
        std::map<int, int> perDrum;
        for (const auto& note : groove.notes) {
            seq.addNote(note.pitch, tracktion::BeatPosition::fromBeats(noteOffset + note.startBeat),
                        tracktion::BeatDuration::fromBeats(note.lengthBeats), note.velocity,
                        kDrumChannel - 1, um);
            perDrum[note.pitch]++;
        }

        int clipIndex = 0;
        for (auto* c : track->getClips()) {
            if (c == midiClip) break;
            ++clipIndex;
        }
        emitEditChanged();
        emitTracksChanged();
        emitMidiClipModified(midiClip);

        out["added"] = groove.notes.size();
        out["clip_index"] = clipIndex;
        out["length_beats"] = lengthBeats;
        out["summary"] = groove.summary;
        QJsonObject counts;
        QStringList missing;
        const auto* drums = findDrums(track);
        for (const auto& [pitch, count] : perDrum) {
            counts[QString("%1 %2").arg(pitch).arg(gmName(pitch))] = count;
            if (drums && !drums->kit().padForNote(pitch)) missing << QString::number(pitch);
        }
        out["notes_per_drum"] = counts;
        if (!missing.isEmpty())
            out["missing_pads"] = QString("The kit has no pad for notes %1, so they are silent; "
                                          "add them with set_drum_pad.").arg(missing.join(", "));
        const int numerator = editMgr_->edit()->tempoSequence.getTimeSigAt(
            tracktion::TimePosition()).numerator;
        if (numerator != beatsPerBar)
            out["time_signature"] = QString("The %1 style is in %2/4 but the song is in %3/4; "
                                            "bars won't line up unless you change it.")
                                        .arg(spec.style).arg(beatsPerBar).arg(numerator);
        return ok(id, out);
    };
}

} // namespace OpenDaw
