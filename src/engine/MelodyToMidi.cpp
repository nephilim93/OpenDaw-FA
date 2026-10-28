#include "MelodyToMidi.h"
#include "music/Theory.h"
#include <cmath>

namespace OpenDaw::MelodyToMidi {

double toBeats(te::Edit& edit, double seconds)
{
    return edit.tempoSequence.toBeats(tracktion::TimePosition::fromSeconds(seconds)).inBeats();
}

QString snapToScale(Melody::Result& result, const QString& scale, std::vector<SnapMove>* moved, QString* error)
{
    const QString name = scale.trimmed().compare("auto", Qt::CaseInsensitive) == 0
                             ? QString::fromStdString(Melody::keyName(result.keyTonic, result.keyMinor))
                             : scale.trimmed();
    const auto parsed = Theory::parseScale(name);
    if (!parsed || parsed->intervals.isEmpty()) {
        if (error) *error = QString("Unknown scale \"%1\": use a key such as \"A minor\", \"Bb major\" or \"D dorian\", or \"auto\".").arg(scale);
        return {};
    }
    for (size_t i = 0; i < result.notes.size(); ++i) {
        auto& n = result.notes[i];
        if (parsed->contains(n.pitch)) continue;
        int up = n.pitch + 1, down = n.pitch - 1;
        while (!parsed->contains(up) && up < 127) ++up;
        while (!parsed->contains(down) && down > 0) --down;
        int to;
        if (up - n.pitch != n.pitch - down) to = up - n.pitch < n.pitch - down ? up : down;
        else to = n.cents >= 0.0 ? up : down;   // as far either way: the way it was sung
        to = std::clamp(to, 0, 127);
        if (moved) moved->push_back({ int(i), n.pitch });
        n.cents += (n.pitch - to) * 100.0;
        n.pitch = to;
    }
    return parsed->name;
}

std::optional<Transcription> transcribe(te::WaveAudioClip& clip, const Melody::Options& options, QString* error,
                                        const QString& snapScale)
{
    const auto file = clip.getOriginalFile();
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(file.existsAsFile() ? formats.createReaderFor(file) : nullptr);
    if (!reader || reader->sampleRate <= 0.0) {
        *error = QString("Can't read the clip's audio file (%1).").arg(QString::fromUtf8(file.getFullPathName().toRawUTF8()));
        return std::nullopt;
    }

    Transcription t;
    const auto pos = clip.getPosition();
    t.clipStartSeconds = pos.getStart().inSeconds();
    t.clipLengthSeconds = pos.getLength().inSeconds();
    const double speed = clip.getSpeedRatio() > 0.0 ? clip.getSpeedRatio() : 1.0;
    if (std::abs(speed - 1.0) > 1.0e-3)
        t.warnings << QString("The clip plays at %1x speed: timing follows the clip, pitch is as recorded.").arg(speed, 0, 'f', 2);
    if (clip.isLooping())
        t.warnings << "The clip loops: only its first pass is transcribed.";

    double seconds = t.clipLengthSeconds * speed;
    if (seconds > kMaxSeconds) {
        t.warnings << QString("Only the first %1 seconds are transcribed.").arg(int(kMaxSeconds));
        seconds = kMaxSeconds;
    }
    const double sr = reader->sampleRate;
    const auto first = juce::int64(std::max(0.0, pos.getOffset().inSeconds() * speed) * sr);
    const auto count = int(std::min<juce::int64>(juce::int64(seconds * sr), reader->lengthInSamples - first));
    if (count <= 0) {
        *error = "The clip has no audio to transcribe.";
        return std::nullopt;
    }
    const int channels = int(std::max(1u, reader->numChannels));
    juce::AudioBuffer<float> buffer(channels, count);
    reader->read(&buffer, 0, count, first, true, channels > 1);
    std::vector<float> mono(size_t(count), 0.0f);
    for (int c = 0; c < channels; ++c) {
        const float* data = buffer.getReadPointer(c);
        for (int i = 0; i < count; ++i) mono[size_t(i)] += data[i] / float(channels);
    }

    t.result = Melody::transcribe(mono, sr, options);
    if (std::abs(speed - 1.0) > 1.0e-3)
        for (auto& n : t.result.notes) {
            n.startSeconds /= speed;
            n.endSeconds /= speed;
        }
    if (!snapScale.isEmpty()) {
        t.snappedTo = snapToScale(t.result, snapScale, &t.snapped, error);
        if (t.snappedTo.isEmpty()) return std::nullopt;
    }
    return t;
}

std::optional<Converted> convert(EditManager& editMgr, te::WaveAudioClip& clip, const Melody::Options& options,
                                 const juce::String& undoName, QString* error, const QString& snapScale,
                                 const Timing& timing)
{
    auto* edit = editMgr.edit();
    if (!edit) {
        *error = "No project is open.";
        return std::nullopt;
    }
    auto transcription = transcribe(clip, options, error, snapScale);
    if (!transcription) return std::nullopt;
    if (transcription->result.notes.empty()) {
        *error = "No melody found: the clip needs humming, singing or whistling (one voice; whispers have no pitch).";
        return std::nullopt;
    }

    auto& um = edit->getUndoManager();
    um.beginNewTransaction(undoName);
    Converted out;
    out.transcription = std::move(*transcription);
    const auto& notes = out.transcription.result.notes;
    out.detected = Melody::estimateTempo(notes);

    // What to do with the tempo.
    TempoMode mode = timing.tempo;
    if (mode == TempoMode::Auto) {
        bool alone = true;
        for (auto* t : te::getAudioTracks(*edit))
            for (auto* c : t->getClips())
                if (c != &clip) alone = false;
        mode = alone ? TempoMode::Set : TempoMode::Fit;
    }
    if (mode != TempoMode::Keep && (out.detected.bpm <= 0.0 || out.detected.confidence < kMinTempoConfidence)) {
        out.transcription.warnings << "The tempo wasn't clear enough to use, so the timing is as sung.";
        mode = TempoMode::Keep;
    }
    out.applied = mode;

    if (mode == TempoMode::Set) {
        // The project takes the sung tempo; the take moves so its first beat starts a bar.
        editMgr.setBpm(std::round(out.detected.bpm * 10.0) / 10.0);
        const double bar = 60.0 / editMgr.getBpm() * edit->tempoSequence.getTimeSigAt(tracktion::TimePosition()).numerator.get();
        const double beatAt = out.transcription.clipStartSeconds + out.detected.firstBeatSeconds;
        double target = std::round(beatAt / bar) * bar;
        if (out.transcription.clipStartSeconds + (target - beatAt) < 0.0) target += bar;
        const double shift = target - beatAt;
        clip.setStart(tracktion::TimePosition::fromSeconds(out.transcription.clipStartSeconds + shift), false, true);
        out.transcription.clipStartSeconds += shift;
    }
    out.projectBpm = editMgr.getBpm();

    out.track = editMgr.addMidiTrack();
    if (!out.track) {
        *error = "Could not add a MIDI track.";
        return std::nullopt;
    }
    juce::String trackName = clip.getTrack() ? clip.getTrack()->getName() : juce::String("Audio");
    out.track->setName(trackName + " Melody");
    editMgr.setTrackModularSynth(*out.track);

    // Each note's start and end on the project's beats.
    const double clipStart = out.transcription.clipStartSeconds;
    std::vector<std::pair<double, double>> beats;
    if (mode == TempoMode::Fit) {
        const double sungBeat = 60.0 / out.detected.bpm;
        const double anchor = std::round(toBeats(*edit, clipStart + out.detected.firstBeatSeconds));
        for (const auto& n : notes)
            beats.push_back({ anchor + (n.startSeconds - out.detected.firstBeatSeconds) / sungBeat,
                              anchor + (n.endSeconds - out.detected.firstBeatSeconds) / sungBeat });
    } else {
        for (const auto& n : notes)
            beats.push_back({ toBeats(*edit, clipStart + n.startSeconds), toBeats(*edit, clipStart + n.endSeconds) });
    }
    if (timing.quantizeBeats > 0.0) {
        const double q = timing.quantizeBeats;
        for (size_t i = 0; i < beats.size(); ++i) {
            auto& [a, b] = beats[i];
            a = std::round(a / q) * q;
            b = std::max(a + q, std::round(b / q) * q);
            if (i > 0) beats[i - 1].second = std::min(beats[i - 1].second, a);   // no overlaps
        }
    }

    double startBeat = std::floor(toBeats(*edit, clipStart) + 1.0e-6);
    double endBeat = toBeats(*edit, clipStart + out.transcription.clipLengthSeconds);
    for (const auto& [a, b] : beats) {
        startBeat = std::min(startBeat, std::floor(a));
        endBeat = std::max(endBeat, b);
    }
    if (mode == TempoMode::Keep && timing.quantizeBeats <= 0.0)
        startBeat = toBeats(*edit, clipStart);   // as before: the clip where the audio is
    out.clip = editMgr.addMidiClipToTrack(*out.track, startBeat, std::max(0.25, endBeat - startBeat));
    if (!out.clip) {
        *error = "Could not add a MIDI clip.";
        return std::nullopt;
    }
    out.clip->setName(clip.getName() + " (melody)");
    out.clipStartBeat = startBeat;
    auto& seq = out.clip->getSequence();
    for (size_t i = 0; i < notes.size(); ++i) {
        const double a = beats[i].first - startBeat, b = beats[i].second - startBeat;
        out.noteBeats.push_back({ std::max(0.0, a), std::max(a + 1.0 / 64.0, b) });
        seq.addNote(notes[i].pitch, tracktion::BeatPosition::fromBeats(std::max(0.0, a)),
                    tracktion::BeatDuration::fromBeats(std::max(1.0 / 64.0, b - a)), notes[i].velocity, 0, &um);
    }
    for (auto* c : out.track->getClips()) {
        if (c == out.clip) break;
        ++out.clipIndex;
    }
    return out;
}

} // namespace OpenDaw::MelodyToMidi
