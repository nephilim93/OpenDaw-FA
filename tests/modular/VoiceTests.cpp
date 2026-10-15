#include "TestHarness.h"

using namespace OpenDaw::Tests;
using OpenDaw::Modular::CompiledPatch;

namespace {

std::unique_ptr<CompiledPatch> compile(const QJsonObject& json)
{
    auto cp = CompiledPatch::build(patchFrom(json), kRate);
    CHECK(cp != nullptr);
    return cp;
}

// Frequency from rising zero crossings (linearly interpolated) of a steady tone.
double measureHz(const std::vector<float>& s, int from, int to)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (s[size_t(i - 1)] < 0.0f && s[size_t(i)] >= 0.0f) {
            const double t = (i - 1) + s[size_t(i - 1)] / double(s[size_t(i - 1)] - s[size_t(i)]);
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
}

// voice_in -> sine vco -> voice_out -> voices -> output at full volume.
QJsonObject sinePatch()
{
    return patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco", { { "wave", "sine" }, { "level", 1 } }),
          mod("vout", "voice_out"), mod("voices", "voices"), mod("out", "output", { { "volume", 1 } }) },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "vout.left"),
          cable("voices.left", "out.left"), cable("voices.right", "out.right") });
}

// Per voice: left = pitch while the gate is high, right = the gate. Summed
// over voices, right counts held notes and left identifies which.
QJsonObject voiceProbePatch()
{
    return patchJson(
        { mod("vin", "voice_in"), mod("amp", "vca"), mod("vout", "voice_out"),
          mod("voices", "voices"), mod("out", "output", { { "volume", 1 } }) },
        { cable("vin.pitch", "amp.in"), cable("vin.gate", "amp.cv"), cable("amp.out", "vout.left"),
          cable("vin.gate", "vout.right"), cable("voices.left", "out.left"),
          cable("voices.right", "out.right") });
}

// Left = the ADSR, right = the voice gate.
QJsonObject envelopePatch(const QJsonObject& adsrParams)
{
    return patchJson(
        { mod("vin", "voice_in"), mod("env", "adsr", adsrParams), mod("vout", "voice_out"),
          mod("voices", "voices"), mod("out", "output", { { "volume", 1 } }) },
        { cable("vin.gate", "env.gate"), cable("env.out", "vout.left"), cable("vin.gate", "vout.right"),
          cable("voices.left", "out.left"), cable("voices.right", "out.right") });
}

} // namespace

// About 2.5 cents. Zero crossings over a second measure a sine to ~0.1%,
// and a looser bound let a 0.4% (7 cent, audibly out of tune) error pass.
constexpr double kPitchTolerance = 0.0015;

TEST(pitch, sine_c4_and_a4)
{
    for (const auto& [note, hz] : { std::pair{ 60, 261.6256 }, std::pair{ 69, 440.0 } }) {
        auto cp = compile(sinePatch());
        if (!cp) return;
        cp->noteOn(note, 1.0f);
        const auto out = renderPatch(*cp, int(kRate));
        const double measured = measureHz(out.left, 480, int(kRate));
        CHECK_MSG(std::abs(measured - hz) <= hz * kPitchTolerance,
                  "note " + str(double(note)) + ": " + str(measured) + " Hz, expected " + str(hz));
        CHECK_NEAR(*std::max_element(out.left.begin(), out.left.end()), 1.0, 0.01);
    }
}

TEST(pitch, pitch_bend_and_vco_tune)
{
    // A4 bent down two semitones is G4; a +7 semitone vco tune on C4 is G4.
    const double g4 = 391.9954;
    auto bent = compile(sinePatch());
    if (!bent) return;
    bent->setPitchBend(-2.0f);
    bent->noteOn(69, 1.0f);
    CHECK_NEAR(measureHz(renderPatch(*bent, int(kRate)).left, 480, int(kRate)), g4, g4 * kPitchTolerance);

    auto json = sinePatch();
    auto modules = json["modules"].toArray();
    modules[1] = mod("osc", "vco", { { "wave", "sine" }, { "tune", 7 } });
    json["modules"] = modules;
    auto tuned = compile(json);
    if (!tuned) return;
    tuned->noteOn(60, 1.0f);
    CHECK_NEAR(measureHz(renderPatch(*tuned, int(kRate)).left, 480, int(kRate)), g4, g4 * kPitchTolerance);
}

TEST(envelope, adsr_shape)
{
    auto cp = compile(envelopePatch({ { "attack", 0.05 }, { "decay", 0.1 }, { "sustain", 0.6 },
                                      { "release", 0.1 } }));
    if (!cp) return;

    // Silent before any note.
    const auto before = renderPatch(*cp, 4800);
    CHECK(*std::max_element(before.left.begin(), before.left.end()) == 0.0f);
    CHECK(*std::min_element(before.left.begin(), before.left.end()) == 0.0f);

    cp->noteOn(60, 1.0f);
    const auto held = renderPatch(*cp, int(kRate * 0.6));
    const auto& env = held.left;

    // Attack: rises steadily, reaches the peak at about the attack time.
    const int attackEnd = int(std::max_element(env.begin(), env.end()) - env.begin());
    CHECK_NEAR(env[size_t(attackEnd)], 1.0, 1e-3);
    CHECK_MSG(attackEnd > int(kRate * 0.04) && attackEnd < int(kRate * 0.06),
              "peak at sample " + str(double(attackEnd)));
    bool rising = true;
    for (int i = 1; i <= attackEnd; ++i) rising = rising && env[size_t(i)] >= env[size_t(i - 1)];
    CHECK(rising);
    CHECK(env[size_t(int(kRate * 0.025))] > 0.2f && env[size_t(int(kRate * 0.025))] < 0.95f);

    // Decay to the sustain level and hold there.
    CHECK_NEAR(env.back(), 0.6, 0.005);
    CHECK_NEAR(env[size_t(int(kRate * 0.5))], 0.6, 0.005);

    // Release: falls to silence.
    cp->noteOff(60);
    const auto released = renderPatch(*cp, int(kRate * 0.5));
    CHECK(released.left[100] < 0.6f && released.left[100] > 0.3f);
    CHECK(released.right[100] == 0.0f);
    CHECK_NEAR(released.left[size_t(int(kRate * 0.1))], 0.0, 0.01);
    CHECK(std::abs(released.left.back()) < 1e-4f);

    const auto after = renderPatch(*cp, int(kRate * 0.5));
    CHECK(*std::max_element(after.left.begin(), after.left.end()) == 0.0f);
}

TEST(feedback, cycle_compiles_and_stays_finite)
{
    // vco -> vcf -> back into the vco's fm: a loop through one cable.
    auto cp = compile(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco", { { "fm_depth", 4 } }),
          mod("f", "vcf", { { "resonance", 0.95 } }), mod("vout", "voice_out"),
          mod("voices", "voices"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "f.in"), cable("f.out", "osc.fm"),
          cable("f.out", "vout.left"), cable("voices.left", "out.left") }));
    if (!cp) return;
    CHECK(cp->numFeedbackCables() == 1);

    for (int note : { 36, 60, 84, 96 }) cp->noteOn(note, 1.0f);
    const auto out = renderPatch(*cp, int(kRate * 2));
    CHECK(allFinite(out.left) && allFinite(out.right));
    CHECK(rms(out.left, 0, int(out.left.size())) > 0.01);

    // A global loop is counted too, and a patch without loops has none.
    auto global = compile(patchJson(
        { mod("a", "slew", {}, "global"), mod("b", "scale", { { "scale", -0.5 } }, "global"),
          mod("out", "output") },
        { cable("a.out", "b.in"), cable("b.out", "a.in"), cable("a.out", "out.left") }));
    if (global) CHECK(global->numFeedbackCables() == 1);
    auto plain = compile(sinePatch());
    if (plain) CHECK(plain->numFeedbackCables() == 0);
}

TEST(voices, steal_oldest_when_full)
{
    auto cp = compile(voiceProbePatch());
    if (!cp) return;
    for (int note = 60; note < 70; ++note) {
        cp->noteOn(note, 1.0f);
        renderPatch(*cp, 64);
    }
    auto out = renderPatch(*cp, 64);
    // 60 and 61 were stolen; 62..69 sound.
    CHECK_NEAR(out.right.back(), 8.0, 1e-4);
    CHECK_NEAR(out.left.back(), (2 + 3 + 4 + 5 + 6 + 7 + 8 + 9) / 12.0, 1e-4);

    // Releasing the stolen notes does nothing; releasing a held one does.
    cp->noteOff(60);
    cp->noteOff(61);
    CHECK_NEAR(renderPatch(*cp, 64).right.back(), 8.0, 1e-4);
    cp->noteOff(69);
    out = renderPatch(*cp, 64);
    CHECK_NEAR(out.right.back(), 7.0, 1e-4);
    CHECK_NEAR(out.left.back(), (2 + 3 + 4 + 5 + 6 + 7 + 8) / 12.0, 1e-4);
}

TEST(voices, steal_released_before_held)
{
    auto cp = compile(voiceProbePatch());
    if (!cp) return;
    for (int note = 60; note < 68; ++note) cp->noteOn(note, 1.0f);
    renderPatch(*cp, 64);
    cp->noteOff(63);
    renderPatch(*cp, 64);

    // All eight voices are busy (63 is still ringing out); the new note takes
    // the released voice, not the oldest held one.
    cp->noteOn(70, 1.0f);
    const auto out = renderPatch(*cp, 64);
    CHECK_NEAR(out.right.back(), 8.0, 1e-4);
    CHECK_NEAR(out.left.back(), (0 + 1 + 2 + 4 + 5 + 6 + 7 + 10) / 12.0, 1e-4);
}

TEST(voices, released_voice_goes_idle)
{
    // A note that steals a busy voice starts with one sub-block of gate low;
    // one that gets an idle voice is gated from its first sample. That tells
    // apart a voice that is still active from one that went idle.
    auto firstSubBlockGate = [](double secondsAfterRelease) {
        auto cp = compile(voiceProbePatch());
        if (!cp) return -1.0f;
        for (int note = 60; note < 68; ++note) cp->noteOn(note, 1.0f);
        renderPatch(*cp, 480);
        for (int note = 60; note < 68; ++note) cp->noteOff(note);
        renderPatch(*cp, int(kRate * secondsAfterRelease));
        cp->noteOn(72, 1.0f);
        return renderPatch(*cp, OpenDaw::Modular::kSubBlock).right.back();
    };
    CHECK_NEAR(firstSubBlockGate(0.05), 0.0, 1e-6);  // still active: stolen
    CHECK_NEAR(firstSubBlockGate(0.5), 1.0, 1e-6);   // idle after 0.3 s of silence
}

TEST(voices, retrigger_restarts_envelope)
{
    auto cp = compile(envelopePatch({ { "attack", 0.05 }, { "decay", 0.05 }, { "sustain", 0.3 },
                                      { "release", 0.5 } }));
    if (!cp) return;
    cp->noteOn(60, 1.0f);
    CHECK_NEAR(renderPatch(*cp, int(kRate * 0.5)).left.back(), 0.3, 0.005);

    // Striking the held note again drops the gate for one sub-block, so the
    // envelope attacks again from where it was.
    cp->noteOn(60, 1.0f);
    const auto out = renderPatch(*cp, int(kRate * 0.1));
    CHECK(out.right[0] == 0.0f);
    CHECK(out.right[size_t(OpenDaw::Modular::kSubBlock)] == 1.0f);
    CHECK_NEAR(*std::max_element(out.left.begin(), out.left.end()), 1.0, 1e-3);

    // Still one voice: the re-strike reused it.
    CHECK_NEAR(out.right.back(), 1.0, 1e-6);
}
