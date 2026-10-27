#include "../modular/TestHarness.h"
#include "music/MelodyTranscriber.h"
#include <cmath>
#include <random>

// Hummed and whistled melodies, synthesized with known notes.

using namespace OpenDaw::Tests;
namespace Mel = OpenDaw::Melody;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct SungNote {
    int pitch;           // MIDI
    double start, end;   // seconds
    bool consonant = true;   // a "t" before it (silence + a noise burst); false: legato
};

struct Voice {
    double sampleRate = 44100.0;
    double detuneCents = 0.0;      // the whole take sharp or flat
    double vibratoCents = 35.0;
    double vibratoHz = 5.5;
    double scoopCents = -80.0;     // slides up into each note
    double scoopSeconds = 0.04;
    bool whistle = false;          // pure tone instead of a voice
    double noiseDb = -60.0;        // background noise
    double level = 0.3;
};

std::vector<float> sing(const std::vector<SungNote>& notes, const Voice& v, double totalSeconds)
{
    const int n = int(totalSeconds * v.sampleRate);
    std::vector<float> out(size_t(n), 0.0f);
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    const float noise = float(std::pow(10.0, v.noiseDb / 20.0));
    for (auto& s : out) s = noise * gauss(rng);

    double phase = 0.0;
    for (size_t k = 0; k < notes.size(); ++k) {
        const auto& note = notes[k];
        const int a = int(note.start * v.sampleRate), b = std::min(n, int(note.end * v.sampleRate));
        if (note.consonant) {
            // "t": 15 ms of noise right before the vowel.
            for (int i = std::max(0, a - int(0.015 * v.sampleRate)); i < a; ++i)
                out[size_t(i)] += 0.15f * gauss(rng);
            phase = 0.0;
        }
        for (int i = a; i < b; ++i) {
            const double t = (i - a) / v.sampleRate;
            const double left = (b - i) / v.sampleRate;
            double cents = v.detuneCents + v.vibratoCents * std::sin(2.0 * kPi * v.vibratoHz * t);
            if (note.consonant && t < v.scoopSeconds) cents += v.scoopCents * (1.0 - t / v.scoopSeconds);
            if (!note.consonant && k > 0 && t < 0.03) {
                // Legato: glide from the previous pitch over 30 ms.
                cents += (notes[k - 1].pitch - note.pitch) * 100.0 * (1.0 - t / 0.03);
            }
            const double hz = 440.0 * std::pow(2.0, (note.pitch - 69 + cents / 100.0) / 12.0);
            phase += 2.0 * kPi * hz / v.sampleRate;
            // Legato joins keep the level: no fade between the two notes.
            const bool joinedBefore = !note.consonant && k > 0;
            const bool joinedAfter = k + 1 < notes.size() && !notes[k + 1].consonant;
            const double env = std::min({ 1.0, joinedBefore ? 1.0 : t / 0.01, joinedAfter ? 1.0 : left / 0.02 });
            double s = 0.0;
            if (v.whistle) {
                s = std::sin(phase);
            } else {
                // A hum: harmonics falling off, a little brighter in the middle.
                for (int h = 1; h <= 10; ++h) s += std::sin(h * phase) / (h * (h > 3 ? 1.6 : 1.0));
                s *= 0.45;
            }
            out[size_t(i)] += float(v.level * env * s);
        }
    }
    return out;
}

void checkNotes(const Mel::Result& r, const std::vector<SungNote>& expected, int shift = 0, double tolerance = 0.035)
{
    if (r.notes.size() != expected.size()) {
        std::string got;
        for (const auto& n : r.notes) got += std::to_string(n.pitch) + "@" + str(n.startSeconds) + " ";
        reportFailure(__FILE__, __LINE__, "expected " + std::to_string(expected.size()) + " notes, got "
                                              + std::to_string(r.notes.size()) + ": " + got);
        return;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& n = r.notes[i];
        const auto& e = expected[i];
        if (n.pitch != e.pitch + shift)
            reportFailure(__FILE__, __LINE__, "note " + std::to_string(i) + ": pitch " + std::to_string(n.pitch)
                                                  + ", expected " + std::to_string(e.pitch + shift));
        if (std::abs(n.startSeconds - e.start) > tolerance)
            reportFailure(__FILE__, __LINE__, "note " + std::to_string(i) + ": starts at " + str(n.startSeconds)
                                                  + ", expected " + str(e.start));
        if (std::abs(n.endSeconds - e.end) > tolerance + 0.02)
            reportFailure(__FILE__, __LINE__, "note " + std::to_string(i) + ": ends at " + str(n.endSeconds)
                                                  + ", expected " + str(e.end));
    }
}

// "Twinkle twinkle": C C G G A A G, each with a "t".
std::vector<SungNote> twinkle(int base = 60, double beat = 0.4)
{
    const int steps[] = { 0, 0, 7, 7, 9, 9, 7 };
    std::vector<SungNote> notes;
    double t = 0.3;
    for (int i = 0; i < 7; ++i) {
        const double len = i == 6 ? 2 * beat : beat;
        notes.push_back({ base + steps[i], t, t + len - 0.06 });
        t += len;
    }
    return notes;
}

} // namespace

TEST(melody, hummed_tune_with_repeated_notes)
{
    const auto notes = twinkle();
    const auto r = Mel::transcribe(sing(notes, {}, 4.0), 44100.0);
    checkNotes(r, notes);   // C C stays two notes: the "t" splits them
    CHECK(std::abs(r.tuningCents) < 10.0);
    CHECK(r.octaveShift == 0);
    for (const auto& n : r.notes) CHECK(n.confidence > 0.6);
    CHECK(Mel::keyName(r.keyTonic, r.keyMinor) == "C major");
}

TEST(melody, off_tune_singer_is_corrected)
{
    // 40 cents sharp throughout: naive rounding would put some notes a
    // semitone high; the offset is removed first.
    Voice v;
    v.detuneCents = 40.0;
    const auto notes = twinkle(57);
    const auto r = Mel::transcribe(sing(notes, v, 4.0), 44100.0);
    checkNotes(r, notes);
    CHECK(std::abs(r.tuningCents - 40.0) < 8.0);
    for (const auto& n : r.notes) CHECK(std::abs(n.cents) < 15.0);

    v.detuneCents = -45.0;
    const auto flat = Mel::transcribe(sing(notes, v, 4.0), 44100.0);
    checkNotes(flat, notes);
    CHECK(std::abs(flat.tuningCents + 45.0) < 8.0);
}

TEST(melody, legato_steps_split_without_a_consonant)
{
    // One breath, no consonants: D E F# G glided into each other.
    std::vector<SungNote> notes = { { 62, 0.3, 0.7 }, { 64, 0.7, 1.1, false }, { 66, 1.1, 1.5, false },
                                    { 67, 1.5, 2.1, false } };
    const auto r = Mel::transcribe(sing(notes, {}, 2.5), 44100.0);
    checkNotes(r, notes, 0, 0.045);
}

TEST(melody, vibrato_and_scoops_are_one_note)
{
    Voice v;
    v.vibratoCents = 60.0;   // wide
    v.scoopCents = -150.0;   // a big slide in
    v.scoopSeconds = 0.06;
    std::vector<SungNote> notes = { { 64, 0.3, 1.5 }, { 69, 1.7, 2.9 } };
    const auto r = Mel::transcribe(sing(notes, v, 3.2), 44100.0);
    checkNotes(r, notes, 0, 0.07);
    // The slide in doesn't make the note look unsteady.
    for (const auto& n : r.notes) CHECK(n.confidence > 0.7);
}

TEST(melody, whistle_comes_down_an_octave)
{
    Voice v;
    v.sampleRate = 48000.0;
    v.whistle = true;
    v.vibratoCents = 15.0;
    v.scoopCents = -40.0;
    const auto notes = twinkle(88, 0.3);   // E6 and up: whistling range
    const auto r = Mel::transcribe(sing(notes, v, 3.0), 48000.0);
    CHECK(r.octaveShift == -12);
    checkNotes(r, notes, -12);
    Mel::Options keep;
    keep.autoOctave = false;
    keep.transpose = -24;
    checkNotes(Mel::transcribe(sing(notes, v, 3.0), 48000.0, keep), notes, -24);
}

TEST(melody, low_hum_and_other_sample_rates)
{
    Voice v;
    v.sampleRate = 48000.0;
    const auto low = twinkle(43);   // G2, a low male hum
    checkNotes(Mel::transcribe(sing(low, v, 4.0), 48000.0), low);
    v.sampleRate = 16000.0;
    const auto mid = twinkle(64);
    checkNotes(Mel::transcribe(sing(mid, v, 4.0), 16000.0), mid);
}

TEST(melody, noisy_room)
{
    Voice v;
    v.noiseDb = -32.0;   // a fan, a laptop: about 20 dB under the voice
    const auto notes = twinkle(60);
    checkNotes(Mel::transcribe(sing(notes, v, 4.0), 44100.0), notes);
}

TEST(melody, silence_breath_and_whispers_give_no_notes)
{
    CHECK(Mel::transcribe(std::vector<float>(44100, 0.0f), 44100.0).notes.empty());
    CHECK(Mel::transcribe({}, 44100.0).notes.empty());
    // A whisper: loud noise, no pitch.
    std::mt19937 rng(3);
    std::normal_distribution<float> gauss(0.0f, 0.2f);
    std::vector<float> whisper(88200);
    for (auto& s : whisper) s = gauss(rng);
    CHECK(Mel::transcribe(whisper, 44100.0).notes.empty());
}

TEST(melody, minor_key_and_velocity)
{
    // A minor: A C E D C B A, the last note softer.
    const int pitches[] = { 69, 72, 76, 74, 72, 71, 69 };
    std::vector<SungNote> notes;
    double t = 0.3;
    for (int p : pitches) {
        notes.push_back({ p, t, t + 0.35 });
        t += 0.42;
    }
    auto audio = sing(notes, {}, 3.6);
    const int lastStart = int(notes.back().start * 44100.0) - 800;
    for (size_t i = size_t(lastStart); i < audio.size(); ++i) audio[i] *= 0.3f;
    const auto r = Mel::transcribe(audio, 44100.0);
    checkNotes(r, notes);
    CHECK(Mel::keyName(r.keyTonic, r.keyMinor) == "A minor");
    CHECK(r.notes.size() == 7 && r.notes.back().velocity < r.notes.front().velocity - 10);
}

TEST(melody, da_da_on_one_pitch_splits_at_the_dip)
{
    // "da da da" on one breath: no silence, the voiced "d" only dips the
    // level for ~30 ms. Three notes, not one long one.
    std::vector<SungNote> notes = { { 65, 0.3, 0.7 }, { 65, 0.7, 1.1, false }, { 65, 1.1, 1.6, false } };
    auto audio = sing(notes, {}, 2.0);
    for (double at : { 0.7, 1.1 })
        for (int i = int((at - 0.015) * 44100.0); i < int((at + 0.015) * 44100.0); ++i) {
            const double x = std::abs(i / 44100.0 - at) / 0.015;   // 0 at the centre
            audio[size_t(i)] *= float(0.08 + 0.92 * x * x);
        }
    checkNotes(Mel::transcribe(audio, 44100.0), notes, 0, 0.03);
}

TEST(melody, quiet_pitched_background_between_phrases_is_ignored)
{
    // A fridge or a monitor hum at 120 Hz, 40 dB under the voice, all along:
    // in the silences it is the only pitched thing, but far too quiet to be a note.
    const auto notes = twinkle(64);
    auto audio = sing(notes, {}, 4.0);
    for (size_t i = 0; i < audio.size(); ++i)
        audio[i] += float(0.003 * std::sin(2.0 * kPi * 120.0 * double(i) / 44100.0));
    checkNotes(Mel::transcribe(audio, 44100.0), notes);
}

TEST(melody, short_blips_are_dropped)
{
    // A 30 ms "hm" between two phrases is not a note.
    std::vector<SungNote> notes = { { 62, 0.3, 0.9 }, { 69, 1.5, 2.1 } };
    std::vector<SungNote> withBlip = { notes[0], { 74, 1.15, 1.18 }, notes[1] };
    checkNotes(Mel::transcribe(sing(withBlip, {}, 2.5), 44100.0), notes);
}

TEST(melody, live_pitch_reading)
{
    Voice v;
    // The last 80 ms of an A3 hum, of a whistled E6, of silence and of a whisper.
    const auto hum = sing({ { 57, 0.0, 0.5 } }, v, 0.5);
    const auto a = Mel::detectPitch(std::vector<float>(hum.end() - 3528 - 2205, hum.end() - 2205), 44100.0);
    CHECK(a.hz > 0.0 && std::abs(69.0 + 12.0 * std::log2(a.hz / 440.0) - 57.0) < 0.5);
    Voice w;
    w.whistle = true;
    w.sampleRate = 48000.0;
    const auto whistle = sing({ { 88, 0.0, 0.5 } }, w, 0.5);
    const auto e = Mel::detectPitch(std::vector<float>(whistle.begin() + 12000, whistle.begin() + 15840), 48000.0);
    CHECK(e.hz > 0.0 && std::abs(69.0 + 12.0 * std::log2(e.hz / 440.0) - 88.0) < 0.5);
    CHECK(Mel::detectPitch(std::vector<float>(3528, 0.0f), 44100.0).hz == 0.0);
    std::mt19937 rng(5);
    std::normal_distribution<float> gauss(0.0f, 0.2f);
    std::vector<float> whisper(3528);
    for (auto& s : whisper) s = gauss(rng);
    CHECK(Mel::detectPitch(whisper, 44100.0).hz == 0.0);
    // A faint pitched sound far under a voice (a fridge, another room): no reading.
    Voice faint;
    faint.level = 0.3 * std::pow(10.0, -48.0 / 20.0);
    faint.noiseDb = -120.0;
    const auto far = sing({ { 57, 0.0, 0.5 } }, faint, 0.5);
    CHECK(Mel::detectPitch(std::vector<float>(far.end() - 3528 - 2205, far.end() - 2205), 44100.0).hz == 0.0);
    // Too short to measure: no pitch rather than a guess.
    CHECK(Mel::detectPitch(std::vector<float>(hum.begin() + 4410, hum.begin() + 4600), 44100.0).hz == 0.0);
}
