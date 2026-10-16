#include "DrumMachinePlugin.h"
#include "drums/SampleLibrary.h"
#include <QJsonDocument>
#include <cmath>
#include <set>

namespace OpenDaw {

namespace {
const juce::Identifier kKitId("drumKit");
constexpr double kMaxSampleSeconds = 60.0;

QJsonObject parseJson(const juce::String& text)
{
    return QJsonDocument::fromJson(QByteArray(text.toRawUTF8())).object();
}

juce::String toJsonString(const Drums::Kit& kit)
{
    const auto bytes = QJsonDocument(kit.toJson()).toJson(QJsonDocument::Compact);
    return juce::String::fromUTF8(bytes.constData(), int(bytes.size()));
}
}

const char* DrumMachinePlugin::xmlTypeName = "openDawDrumMachine";

DrumMachinePlugin::DrumMachinePlugin(te::PluginCreationInfo info)
    : te::Plugin(info)
{
    kitJson_.referTo(state, kKitId, getUndoManager());
    // A new instrument starts from the default kit (not an undo step).
    if (kitJson_.get().isEmpty())
        kitJson_.setValue(toJsonString(Drums::Kit::defaultKit()), nullptr);
    loadKitFromState();
}

DrumMachinePlugin::~DrumMachinePlugin()
{
    notifyListenersOfDeletion();
    const juce::SpinLock::ScopedLockType lock(engineLock_);
    engine_.reset();
}

void DrumMachinePlugin::setKit(const Drums::Kit& kit, const juce::String& undoName)
{
    if (auto* um = getUndoManager(); um && undoName.isNotEmpty())
        um->beginNewTransaction(undoName);
    kitJson_ = toJsonString(kit);  // valueTreePropertyChanged rebuilds
}

void DrumMachinePlugin::auditionNote(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    const auto scope = auditionFifo_.write(1);
    if (scope.blockSize1 > 0)
        auditions_[size_t(scope.startIndex1)] = { note, juce::jlimit(0.0f, 1.0f, velocity),
                                                  std::chrono::steady_clock::now() };
}

void DrumMachinePlugin::loadKitFromState()
{
    QString error;
    auto parsed = Drums::Kit::fromJson(parseJson(kitJson_.get()), &error);
    if (!parsed)
        juce::Logger::writeToLog("Drum Machine: the stored kit is invalid ("
                                 + juce::String(error.toStdString()) + "); using the default kit.");
    kit_ = parsed ? *parsed : Drums::Kit::defaultKit();
    rebuild();
}

std::shared_ptr<const Drums::SampleData> DrumMachinePlugin::loadSample(const QString& path)
{
    if (auto it = sampleCache_.find(path); it != sampleCache_.end()) return it->second;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File file(juce::String(path.toStdString()));
    std::unique_ptr<juce::AudioFormatReader> reader(file.existsAsFile() ? formats.createReaderFor(file) : nullptr);
    if (!reader || reader->lengthInSamples < 2 || reader->sampleRate <= 0.0) return nullptr;

    const int length = int(std::min<juce::int64>(reader->lengthInSamples,
                                                 juce::int64(kMaxSampleSeconds * reader->sampleRate)));
    const int channels = int(std::min(2u, reader->numChannels));
    juce::AudioBuffer<float> buffer(channels, length);
    reader->read(&buffer, 0, length, 0, true, channels > 1);

    auto data = std::make_shared<Drums::SampleData>();
    data->sampleRate = reader->sampleRate;
    data->left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + length);
    if (channels > 1) data->right.assign(buffer.getReadPointer(1), buffer.getReadPointer(1) + length);
    sampleCache_[path] = data;
    return data;
}

void DrumMachinePlugin::rebuild()
{
    Drums::SampleMap samples;
    // Pads name their files as the kit stores them (relative to the samples
    // folder, or absolute); the cache is keyed by the file actually read.
    std::set<QString> used;
    const QString folder = Drums::samplesFolder();
    for (const auto& pad : kit_.pads)
        if (pad.source == Drums::PadSource::Sample) {
            const QString file = Drums::resolveSamplePath(pad.sample, folder);
            used.insert(file);
            if (auto data = loadSample(file)) samples[pad.sample] = data;
        }
    // Forget files no pad uses any more.
    for (auto it = sampleCache_.begin(); it != sampleCache_.end();)
        it = used.count(it->first) ? std::next(it) : sampleCache_.erase(it);

    QStringList warnings;
    auto fresh = Drums::DrumEngine::build(kit_, sampleRate_, samples, &warnings);
    warnings_ = warnings;
    for (const auto& w : warnings)
        juce::Logger::writeToLog("Drum Machine: " + juce::String(w.toStdString()));
    {
        const juce::SpinLock::ScopedLockType lock(engineLock_);
        std::swap(engine_, fresh);
    }
    // `fresh` now holds the old engine and is freed here, off the audio thread.
}

void DrumMachinePlugin::reloadSamples()
{
    sampleCache_.clear();
    rebuild();
}

void DrumMachinePlugin::initialise(const te::PluginInitialisationInfo& info)
{
    if (info.sampleRate > 0.0 && std::abs(info.sampleRate - sampleRate_) > 0.5) {
        sampleRate_ = info.sampleRate;
        rebuild();
    }
}

void DrumMachinePlugin::getChannelNames(juce::StringArray* ins, juce::StringArray* outs)
{
    if (ins) ins->clear();
    if (outs) { outs->clear(); outs->add("Left"); outs->add("Right"); }
}

void DrumMachinePlugin::valueTreePropertyChanged(juce::ValueTree& v, const juce::Identifier& id)
{
    te::Plugin::valueTreePropertyChanged(v, id);
    // te::Plugin's listener runs before kitJson_'s own, so on undo/redo the
    // cached value is still the old JSON unless refreshed here.
    if (v == state && id == kKitId) {
        kitJson_.forceUpdateOfCachedValue();
        loadKitFromState();
    }
}

void DrumMachinePlugin::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    if (v.hasProperty(kKitId))
        kitJson_.setValue(v.getProperty(kKitId).toString(), nullptr);
    loadKitFromState();
}

void DrumMachinePlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    auto* dest = fc.destBuffer;
    const int numSamples = fc.bufferNumSamples;
    if (dest)
        for (int ch = 0; ch < dest->getNumChannels(); ++ch)
            dest->clear(ch, fc.bufferStartSample, numSamples);

    const juce::SpinLock::ScopedTryLockType lock(engineLock_);
    if (!lock.isLocked() || !engine_) return;  // a kit swap is in progress
    auto& engine = *engine_;

    // Auditions wait in the queue while a kit swap holds the lock. Ones
    // queued while the audio was stopped would all fire at once: skip them.
    {
        const auto scope = auditionFifo_.read(auditionFifo_.getNumReady());
        const auto oldest = std::chrono::steady_clock::now() - std::chrono::milliseconds(kAuditionMaxAgeMs);
        auto play = [&](int start, int count) {
            for (int i = start; i < start + count; ++i)
                if (const auto& a = auditions_[size_t(i)]; a.queuedAt >= oldest)
                    engine.noteOn(a.note, a.velocity);
        };
        play(scope.startIndex1, scope.blockSize1);
        play(scope.startIndex2, scope.blockSize2);
    }

    auto renderRange = [&](int from, int to) {
        while (from < to) {
            const int n = std::min(kRenderChunk, to - from);
            engine.render(renderL_.data(), renderR_.data(), n);
            if (dest) {
                // Gentle limiter: transparent at normal levels, never above 1.
                for (int i = 0; i < n; ++i) {
                    if (dest->getNumChannels() > 0)
                        dest->setSample(0, fc.bufferStartSample + from + i, std::tanh(renderL_[size_t(i)]));
                    if (dest->getNumChannels() > 1)
                        dest->setSample(1, fc.bufferStartSample + from + i, std::tanh(renderR_[size_t(i)]));
                }
            }
            from += n;
        }
    };

    // Hits start at their exact sample: render up to each MIDI event.
    int position = 0;
    if (fc.bufferForMidiMessages) {
        fc.bufferForMidiMessages->sortByTimestamp();
        for (auto& m : *fc.bufferForMidiMessages) {
            const int at = juce::jlimit(0, numSamples, juce::roundToInt(m.getTimeStamp() * sampleRate_));
            renderRange(position, at);
            position = std::max(position, at);
            if (m.isNoteOn())
                engine.noteOn(m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                engine.allNotesOff();
        }
    }
    renderRange(position, numSamples);
}

} // namespace OpenDaw
