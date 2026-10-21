#include "MelodyToMidi.h"
#include <cmath>

namespace OpenDaw::MelodyToMidi {

double toBeats(te::Edit& edit, double seconds)
{
    return edit.tempoSequence.toBeats(tracktion::TimePosition::fromSeconds(seconds)).inBeats();
}

std::optional<Transcription> transcribe(te::WaveAudioClip& clip, const Melody::Options& options, QString* error)
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
    return t;
}

std::optional<Converted> convert(EditManager& editMgr, te::WaveAudioClip& clip, const Melody::Options& options,
                                 const juce::String& undoName, QString* error)
{
    auto* edit = editMgr.edit();
    if (!edit) {
        *error = "No project is open.";
        return std::nullopt;
    }
    auto transcription = transcribe(clip, options, error);
    if (!transcription) return std::nullopt;
    if (transcription->result.notes.empty()) {
        *error = "No melody found: the clip needs humming, singing or whistling (one voice; whispers have no pitch).";
        return std::nullopt;
    }

    auto& um = edit->getUndoManager();
    um.beginNewTransaction(undoName);
    Converted out;
    out.transcription = std::move(*transcription);
    out.track = editMgr.addMidiTrack();
    if (!out.track) {
        *error = "Could not add a MIDI track.";
        return std::nullopt;
    }
    juce::String trackName = clip.getTrack() ? clip.getTrack()->getName() : juce::String("Audio");
    out.track->setName(trackName + " Melody");
    editMgr.setTrackModularSynth(*out.track);

    const double clipStart = out.transcription.clipStartSeconds;
    const double startBeat = toBeats(*edit, clipStart);
    const double endBeat = toBeats(*edit, clipStart + out.transcription.clipLengthSeconds);
    out.clip = editMgr.addMidiClipToTrack(*out.track, startBeat, std::max(0.25, endBeat - startBeat));
    if (!out.clip) {
        *error = "Could not add a MIDI clip.";
        return std::nullopt;
    }
    out.clip->setName(clip.getName() + " (melody)");
    auto& seq = out.clip->getSequence();
    for (const auto& n : out.transcription.result.notes) {
        const double a = toBeats(*edit, clipStart + n.startSeconds) - startBeat;
        const double b = toBeats(*edit, clipStart + n.endSeconds) - startBeat;
        seq.addNote(n.pitch, tracktion::BeatPosition::fromBeats(std::max(0.0, a)),
                    tracktion::BeatDuration::fromBeats(std::max(1.0 / 64.0, b - a)), n.velocity, 0, &um);
    }
    for (auto* c : out.track->getClips()) {
        if (c == out.clip) break;
        ++out.clipIndex;
    }
    return out;
}

} // namespace OpenDaw::MelodyToMidi
