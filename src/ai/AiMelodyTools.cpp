#include "AiToolDefs.h"
#include "AiToolExecutor.h"
#include "engine/EditManager.h"
#include "engine/MelodyToMidi.h"
#include <QJsonArray>
#include <map>
#include <cmath>

// AI tools for melodies hummed, sung or whistled into an audio clip: the
// transcription itself lives in src/music/MelodyTranscriber and
// src/engine/MelodyToMidi; this handler resolves the clip, writes the raw
// notes as one undo step, and reports them with what the model needs to
// clean them up (confidence, pitch error, key and tuning estimates).

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

QString noteName(int midi)
{
    static const char* kNames[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return QString("%1%2").arg(kNames[midi % 12]).arg(midi / 12 - 1);
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

constexpr double kLowConfidence = 0.55;

} // namespace

QJsonArray AiToolDefs::melodyTools()
{
    QJsonArray tools;
    QJsonObject props{
        { "track", trackRef() },
        { "clip_index", QJsonObject{ { "type", "integer" }, { "description", "The audio clip's index on that track (get_clips_on_track)." } } },
        { "transpose", QJsonObject{ { "type", "integer" }, { "minimum", -36 }, { "maximum", 36 },
                                    { "description", "Semitones added to every note (default 0)." } } },
        { "auto_octave", propOf("boolean", "Bring a whistle (above C6) or a very low hum (below C2) into range by octaves (default true).") },
        { "snap_to_scale", propOf("string", "Move notes outside a scale to the nearest scale note: \"auto\" for the "
                                            "detected key, or a scale such as \"A minor\", \"D dorian\", \"C minor pentatonic\". "
                                            "Omit to keep the notes as sung.") },
    };
    tools.append(QJsonObject{
        { "name", "transcribe_melody" },
        { "description",
          "Turn a hummed, sung (\"ta da da\") or whistled melody in an audio clip into MIDI notes, as one "
          "undoable step: a new MIDI track named '<track> Melody' (with the Modular Synth) gets a clip at the "
          "audio clip's place. The result lists every note with its start and length in beats (from the clip "
          "start), a confidence (0-1) and how many cents off pitch it was sung, plus the singer's tuning "
          "offset, an estimated key and the tempo. The raw notes follow the performance exactly: clean them "
          "up afterwards (see the system prompt)." },
        { "input_schema", QJsonObject{ { "type", "object" }, { "properties", props },
                                       { "required", QJsonArray{ "track", "clip_index" } } } },
    });
    return tools;
}

void AiToolExecutor::registerMelodyHandlers()
{
    handlers_["transcribe_melody"] = [this](const QJsonObject& input, const QString& id) -> AiToolResult {
        auto* track = resolveTrack(input["track"]);
        if (!track) return err(id, "Track not found.");
        const auto clips = track->getClips();
        const int index = input["clip_index"].toInt(-1);
        if (index < 0 || index >= clips.size())
            return err(id, QString("clip_index %1 is out of range (the track has %2 clips).").arg(index).arg(clips.size()));
        auto* audio = dynamic_cast<te::WaveAudioClip*>(clips[index]);
        if (!audio) return err(id, QString("Clip %1 is not an audio clip.").arg(index));

        Melody::Options options;
        options.transpose = std::clamp(input["transpose"].toInt(0), -36, 36);
        options.autoOctave = input["auto_octave"].toBool(true);
        QString error;
        const QString snap = input["snap_to_scale"].toString();
        auto converted = MelodyToMidi::convert(*editMgr_, *audio, options, "AI: Transcribe Melody", &error, snap);
        if (!converted) return err(id, error + " Nothing was changed.");

        auto& edit = *editMgr_->edit();
        const auto& r = converted->transcription.result;
        const double clipStart = converted->transcription.clipStartSeconds;
        const double startBeat = MelodyToMidi::toBeats(edit, clipStart);
        QJsonArray notes, low;
        std::map<int, int> snappedFrom;
        for (const auto& m : converted->transcription.snapped) snappedFrom[m.index] = m.fromPitch;
        for (size_t i = 0; i < r.notes.size(); ++i) {
            const auto& n = r.notes[i];
            const double a = MelodyToMidi::toBeats(edit, clipStart + n.startSeconds) - startBeat;
            const double b = MelodyToMidi::toBeats(edit, clipStart + n.endSeconds) - startBeat;
            notes.append(QJsonObject{ { "i", int(i) }, { "note", n.pitch }, { "name", noteName(n.pitch) },
                                      { "start_beat", round3(a) }, { "length_beats", round3(b - a) },
                                      { "velocity", n.velocity }, { "confidence", round3(n.confidence) },
                                      { "cents", int(std::lround(n.cents)) } });
            if (auto it = snappedFrom.find(int(i)); it != snappedFrom.end()) {
                auto o = notes.last().toObject();
                o["snapped_from"] = noteName(it->second);
                notes.replace(notes.size() - 1, o);
            }
            if (n.confidence < kLowConfidence) low.append(int(i));
        }

        int trackIndex = 0;
        for (auto* t : te::getAudioTracks(edit)) {
            if (t == converted->track) break;
            ++trackIndex;
        }
        const auto& tempo = edit.tempoSequence;
        QJsonObject out{
            { "midi_track", QString::fromUtf8(converted->track->getName().toRawUTF8()) },
            { "midi_track_index", trackIndex },
            { "clip_index", converted->clipIndex },
            { "clip_start_beat", round3(startBeat) },
            { "tempo_bpm", round3(tempo.getBpmAt(tracktion::TimePosition::fromSeconds(clipStart))) },
            { "beats_per_bar", tempo.getTimeSigAt(tracktion::TimePosition::fromSeconds(clipStart)).numerator.get() },
            { "tuning_cents", int(std::lround(r.tuningCents)) },
            { "octave_shift", r.octaveShift },
            { "key_estimate", QString::fromStdString(Melody::keyName(r.keyTonic, r.keyMinor)) },
            { "key_confidence", round3(r.keyConfidence) },
            { "notes", notes },
        };
        if (!low.isEmpty()) out["low_confidence"] = low;
        if (!converted->transcription.snappedTo.isEmpty()) {
            out["snapped_to"] = converted->transcription.snappedTo;
            out["snapped_count"] = int(converted->transcription.snapped.size());
        }
        if (!converted->transcription.warnings.isEmpty())
            out["warnings"] = QJsonArray::fromStringList(converted->transcription.warnings);
        emitTracksChanged();
        emitEditChanged();
        emitMidiClipModified(converted->clip);
        return ok(id, out);
    };
}

} // namespace OpenDaw
