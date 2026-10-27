#pragma once

#include <string>
#include <vector>

// Hummed, sung ("ta da da") or whistled melody -> notes. One voice at a
// time; no Qt or JUCE here (like the rest of src/music), so it is testable
// on its own. Whispers have no pitch and give no notes.
namespace OpenDaw::Melody {

struct Options {
    double minHz = 55.0;      // a low hum
    double maxHz = 3000.0;    // a high whistle
    double minNoteSeconds = 0.06;
    // Semitones added to every note. With autoOctave, whole octaves are
    // added on top when the melody's middle is outside C2..C6: a whistle
    // comes down an octave or two, an ordinary hum stays where it is.
    int transpose = 0;
    bool autoOctave = true;
};

struct Frame {
    double seconds = 0.0;
    double hz = 0.0;           // 0: no pitch (silence, breath, consonant)
    double confidence = 0.0;   // 0..1, how periodic
    double db = -120.0;        // loudness (RMS, dBFS)
};

struct Note {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    int pitch = 60;            // MIDI, after tuning correction and octave shift
    double cents = 0.0;        // how far the sung pitch was from it, after tuning correction
    double confidence = 0.0;   // 0..1: steady, clearly pitched notes score high
    int velocity = 90;         // from loudness
};

struct Result {
    std::vector<Note> notes;
    std::vector<Frame> frames;
    double tuningCents = 0.0;  // the singer's overall offset from A440, removed from every note
    int octaveShift = 0;       // semitones autoOctave added (a multiple of 12)
    // Duration-weighted key estimate (Krumhansl profiles): tonic 0 = C.
    int keyTonic = 0;
    bool keyMinor = false;
    double keyConfidence = 0.0;  // correlation, -1..1
};

Result transcribe(const std::vector<float>& mono, double sampleRate, const Options& options = {});

// One live reading from the end of `recent` (the last ~60 ms of input are
// enough): the pitch now, for a "you are singing A4" readout. hz is 0 for
// silence, breath or whispers. No tuning correction or octave shift.
Frame detectPitch(const std::vector<float>& recent, double sampleRate, const Options& options = {});

// "A minor", "C# major".
std::string keyName(int tonic, bool minor);

} // namespace OpenDaw::Melody
