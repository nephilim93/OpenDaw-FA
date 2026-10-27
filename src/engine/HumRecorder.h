#pragma once

#include "engine/EditManager.h"
#include "music/MelodyTranscriber.h"
#include <QObject>
#include <QTimer>
#include <atomic>
#include <map>
#include <memory>

namespace OpenDaw {

// Taps one wave input device straight from the audio device, for a live
// "what am I singing" readout. Lock-free: the audio thread writes a FIFO,
// the message thread drains it.
class LivePitchMonitor : public juce::AudioIODeviceCallback {
public:
    explicit LivePitchMonitor(te::Engine& engine);
    ~LivePitchMonitor() override;

    // Listens to the first channel of the named wave input; false when there
    // is no such input or no open audio device.
    bool start(const juce::String& waveInputName);
    void stop();
    bool isRunning() const { return running_; }

    // The latest reading, from the last ~80 ms of input.
    Melody::Frame read();

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                          float* const* outputs, int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

private:
    te::Engine& engine_;
    bool running_ = false;
    int deviceChannel_ = -1;                 // in the device's channel numbering
    std::atomic<int> callbackChannel_ { -1 }; // among the active channels the callback gets
    std::atomic<double> sampleRate_ { 0.0 };
    juce::AbstractFifo fifo_ { 1 << 15 };
    std::vector<float> ring_ = std::vector<float>(1 << 15);
    std::vector<float> recent_;
};

// The "Hum" button: records a take on a Hum track after a count-in, shows
// the pitch while you sing, and hands the take to Convert to MIDI
// (EditManager::melodyConversionRequested) when recording stops.
class HumRecorder : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, CountIn, Recording };

    explicit HumRecorder(EditManager& editMgr, QObject* parent = nullptr);
    ~HumRecorder() override;

    // Sets up the track and starts recording; false (with a reason) when it
    // can't, such as with no audio input.
    bool start(QString* error = nullptr);
    void stop();
    State state() const { return state_; }
    te::AudioTrack* track() const;

    // Steps of start(), public for tests: picks or makes the Hum track, gives
    // it an input and arms only it (other tracks' arming is restored later).
    te::AudioTrack* prepareTrack(QString* error);
    // A recorded clip arrived: converts it when it is the hum take.
    void takeRecorded(const QList<te::Clip*>& clips);

    static QString noteName(double hz);   // "A4 +12¢"

signals:
    void stateChanged(OpenDaw::HumRecorder::State state);
    // "Count-in 3", "A4 +12¢", "…" (no pitch), or empty when idle.
    void readoutChanged(const QString& text);
    void failed(const QString& message);

private:
    void poll();
    void restore();
    void setState(State s);

    EditManager& editMgr_;
    std::unique_ptr<LivePitchMonitor> monitor_;
    QTimer timer_;
    State state_ = State::Idle;
    te::EditItemID trackId_;
    std::map<uint64_t, bool> armedBefore_;   // track id -> armed
    std::optional<te::Edit::CountIn> countInBefore_;
    double startSeconds_ = 0.0;
    QString lastReadout_;
};

} // namespace OpenDaw
