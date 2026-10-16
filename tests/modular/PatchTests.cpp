#include "TestHarness.h"

using namespace OpenDaw::Tests;
using OpenDaw::Modular::Patch;

namespace {

QString fromJsonError(const QJsonObject& json)
{
    QString error;
    const auto patch = Patch::fromJson(json, &error);
    CHECK_MSG(!patch.has_value(), "expected the patch to be rejected");
    return error;
}

QString compact(const QJsonObject& json)
{
    return QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));
}

// A patch that uses a global module, a choice param and a macro.
QJsonObject richPatchJson()
{
    auto json = patchJson(
        { mod("vin", "voice_in"),
          mod("osc", "vco", { { "wave", "sine" }, { "tune", 7 } }),
          mod("vout", "voice_out"),
          mod("voices", "voices"),
          mod("lfo1", "lfo", { { "wave", "square" }, { "rate", 0.5 } }, "global"),
          mod("echo", "delay", { { "sync", "1/8d" }, { "feedback", 0.6 } }),
          mod("out", "output", { { "volume", 0.5 } }) },
        { cable("vin.pitch", "osc.pitch"), cable("lfo1.out", "osc.fm"),
          cable("osc.out", "vout.left"), cable("voices.left", "echo.left"),
          cable("echo.left", "out.left"), cable("echo.right", "out.right") });
    json["macros"] = QJsonArray{ QJsonObject{
        { "name", "Echo" },
        { "targets", QJsonArray{ QJsonObject{ { "module", "echo" }, { "param", "mix" },
                                              { "min", 0.1 }, { "max", 0.9 } } } } } };
    return json;
}

} // namespace

TEST(patch, json_round_trip)
{
    for (const auto& original : { Patch::initPatch().toJson(), richPatchJson() }) {
        const Patch first = patchFrom(original);
        const QJsonObject json = first.toJson();
        const Patch second = patchFrom(json);
        CHECK_MSG(second.toJson() == json, compact(json) + " vs " + compact(second.toJson()));
        CHECK(second.modules.size() == first.modules.size());
        CHECK(second.cables == first.cables);
    }

    const Patch rich = patchFrom(richPatchJson());
    const auto json = rich.toJson();
    // Choices are written by name, sections explicitly, and macros survive.
    const auto osc = json["modules"].toArray()[1].toObject();
    CHECK(osc["params"].toObject()["wave"].toString() == "sine");
    CHECK(osc["section"].toString() == "voice");
    CHECK(json["modules"].toArray()[4].toObject()["section"].toString() == "global");
    CHECK(rich.find("echo")->params.value("sync") == 3.0f);
    CHECK(rich.macros[0].name == "Echo");
    CHECK(rich.macros[0].targets.size() == 1);
    CHECK_NEAR(rich.macros[0].targets[0].max, 0.9, 1e-6);
}

TEST(patch, validation_unknown_type)
{
    const QString err = fromJsonError(patchJson({ mod("x", "wobbulator") }, {}));
    CHECK_MSG(err.contains("Unknown module type 'wobbulator'"), err);
    CHECK_MSG(err.contains("Available:") && err.contains("vco") && err.contains("reverb"), err);

    Patch p = Patch::initPatch();
    const QString opErr = OpenDaw::Modular::applyOps(
        p, { QJsonObject{ { "op", "add_module" }, { "id", "w" }, { "type", "wobbulator" } } });
    CHECK_MSG(opErr.contains("Unknown module type 'wobbulator'") && opErr.contains("Available:"), opErr);
}

TEST(patch, validation_unknown_port)
{
    QString err = fromJsonError(patchJson({ mod("osc", "vco"), mod("f", "vcf") },
                                          { cable("osc.out", "f.input") }));
    CHECK_MSG(err.contains("has no input 'input'"), err);
    CHECK_MSG(err.contains("Inputs: in, cutoff, pitch"), err);

    err = fromJsonError(patchJson({ mod("osc", "vco"), mod("f", "vcf") },
                                  { cable("osc.output", "f.in") }));
    CHECK_MSG(err.contains("has no output 'output'"), err);
    CHECK_MSG(err.contains("Outputs: out"), err);

    err = fromJsonError(patchJson({ mod("osc", "vco") }, { cable("osc.out", "ghost.in") }));
    CHECK_MSG(err.contains("doesn't exist"), err);
}

TEST(patch, validation_unknown_param)
{
    const QString err = fromJsonError(patchJson({ mod("osc", "vco", { { "detune", 3 } }) }, {}));
    CHECK_MSG(err.contains("has no parameter 'detune'"), err);
    CHECK_MSG(err.contains("Parameters: wave, octave, tune, fine"), err);

    Patch p = Patch::initPatch();
    const QString opErr = OpenDaw::Modular::applyOps(
        p, { QJsonObject{ { "op", "set_param" }, { "module", "vcf1" }, { "param", "q" },
                          { "value", 0.5 } } });
    CHECK_MSG(opErr.contains("has no parameter 'q'") && opErr.contains("resonance"), opErr);
}

TEST(patch, validation_out_of_range)
{
    const QString err = fromJsonError(patchJson({ mod("f", "vcf", { { "cutoff", 50000 } }) }, {}));
    CHECK_MSG(err.contains("cutoff = 50000 is out of range (20 to 20000 Hz)"), err);

    Patch p = Patch::initPatch();
    const QString opErr = OpenDaw::Modular::applyOps(
        p, { QJsonObject{ { "op", "set_param" }, { "module", "vca1" }, { "param", "gain" },
                          { "value", 2 } } });
    CHECK_MSG(opErr.contains("out of range (0 to 1)"), opErr);
}

TEST(patch, validation_bad_choice)
{
    const QString err = fromJsonError(patchJson({ mod("osc", "vco", { { "wave", "sinus" } }) }, {}));
    CHECK_MSG(err.contains("'sinus' is not a valid wave"), err);
    CHECK_MSG(err.contains("Use one of: saw, square, triangle, sine"), err);

    // Choice names are case-insensitive.
    const Patch ok = patchFrom(patchJson({ mod("osc", "vco", { { "wave", "SINE" } }) }, {}));
    CHECK(ok.modules.size() == 1 && ok.modules[0].params.value("wave") == 3.0f);
}

TEST(patch, validation_duplicate_id)
{
    const QString err = fromJsonError(patchJson({ mod("osc", "vco"), mod("osc", "noise") }, {}));
    CHECK_MSG(err.contains("Two modules share the id 'osc'"), err);

    Patch p = Patch::initPatch();
    const QString opErr = OpenDaw::Modular::applyOps(
        p, { QJsonObject{ { "op", "add_module" }, { "id", "vco1" }, { "type", "vco" } } });
    CHECK_MSG(opErr.contains("'vco1' already exists") && opErr.contains("pick another id"), opErr);
}

TEST(patch, validation_voice_to_global)
{
    const QString err = fromJsonError(patchJson(
        { mod("osc", "vco"), mod("out", "output") }, { cable("osc.out", "out.left") }));
    CHECK_MSG(err.contains("goes from a voice to the global section"), err);
    CHECK_MSG(err.contains("voice_out -> voices"), err);

    // Global to voice is allowed (e.g. a shared LFO).
    patchFrom(patchJson({ mod("lfo1", "lfo", {}, "global"), mod("osc", "vco") },
                        { cable("lfo1.out", "osc.fm") }));
}

TEST(patch, apply_ops_all_or_nothing)
{
    Patch p = Patch::initPatch();
    const QJsonObject before = p.toJson();

    // The third op fails: the first two must not stick.
    QString err = OpenDaw::Modular::applyOps(p, {
        QJsonObject{ { "op", "add_module" }, { "id", "lfo1" }, { "type", "lfo" } },
        QJsonObject{ { "op", "connect" }, { "from", "lfo1.out" }, { "to", "vco1.fm" } },
        QJsonObject{ { "op", "set_param" }, { "module", "vco1" }, { "param", "fm_depth" },
                     { "value", 99 } } });
    CHECK_MSG(err.startsWith("Op 3 (set_param)"), err);
    CHECK_MSG(err.endsWith("No changes were made."), err);
    CHECK(p.toJson() == before);

    // Every op succeeds but the result is invalid (caught by validate()).
    err = OpenDaw::Modular::applyOps(p, {
        QJsonObject{ { "op", "remove_module" }, { "id", "vca1" } },
        QJsonObject{ { "op", "connect" }, { "from", "vcf1.out" }, { "to", "voice_out.middle" } } });
    CHECK_MSG(err.contains("has no input 'middle'") && err.endsWith("No changes were made."), err);
    CHECK(p.toJson() == before);

    // Unknown ops list the valid ones.
    err = OpenDaw::Modular::applyOps(p, { QJsonObject{ { "op", "explode" } } });
    CHECK_MSG(err.contains("Unknown op 'explode'") && err.contains("add_module"), err);
    CHECK(p.toJson() == before);

    // A valid batch applies completely.
    err = OpenDaw::Modular::applyOps(p, {
        QJsonObject{ { "op", "add_module" }, { "id", "lfo1" }, { "type", "lfo" } },
        QJsonObject{ { "op", "connect" }, { "from", "lfo1.out" }, { "to", "vco1.fm" } },
        QJsonObject{ { "op", "set_param" }, { "module", "vco1" }, { "param", "wave" },
                     { "value", "square" } } });
    CHECK_MSG(err.isEmpty(), err);
    CHECK(p.find("lfo1") != nullptr);
    CHECK(p.cables.contains(OpenDaw::Modular::Cable{ { "lfo1", "out" }, { "vco1", "fm" } }));
    CHECK(p.find("vco1")->params.value("wave") == 1.0f);

    // Removing a module takes its cables with it.
    err = OpenDaw::Modular::applyOps(p, { QJsonObject{ { "op", "remove_module" }, { "id", "lfo1" } } });
    CHECK_MSG(err.isEmpty(), err);
    for (const auto& c : p.cables) CHECK(c.from.module != "lfo1" && c.to.module != "lfo1");
}

TEST(patch, warnings)
{
    CHECK(Patch::initPatch().warnings().isEmpty());
    CHECK(Patch::initPatch().validate().isEmpty());

    auto has = [](const QStringList& list, const char* text) {
        for (const auto& w : list) if (w.contains(text)) return true;
        return false;
    };

    const QStringList none = Patch{}.warnings();
    CHECK_MSG(has(none, "No 'output' module"), none.join(" | "));

    const QStringList unfed = patchFrom(patchJson({ mod("out", "output") }, {})).warnings();
    CHECK_MSG(has(unfed, "left input has no cable"), unfed.join(" | "));

    const QStringList noVoiceOut = patchFrom(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco"), mod("voices", "voices"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("voices.left", "out.left") })).warnings();
    CHECK_MSG(has(noVoiceOut, "no 'voice_out'"), noVoiceOut.join(" | "));
    CHECK(!has(noVoiceOut, "No 'output'"));

    const QStringList noVoices = patchFrom(patchJson(
        { mod("osc", "vco"), mod("vout", "voice_out"), mod("out", "output") },
        { cable("osc.out", "vout.left") })).warnings();
    CHECK_MSG(has(noVoices, "no global 'voices'"), noVoices.join(" | "));

    // A global sound into voice_out is only heard while notes play.
    const QStringList globalIntoVoice = patchFrom(patchJson(
        { mod("bd", "noise", {}, "global"),
          mod("vout", "voice_out"), mod("voices", "voices", {}, "global"), mod("out", "output") },
        { cable("bd.out", "vout.left"),
          cable("voices.left", "out.left") })).warnings();
    CHECK_MSG(has(globalIntoVoice, "'bd' is in the global section but feeds vout.left"),
              globalIntoVoice.join(" | "));
    // Global modulation into a voice module is fine: no warning for it.
    const QStringList modulation = patchFrom(patchJson(
        { mod("vin", "voice_in"), mod("lfo", "lfo", {}, "global"), mod("osc", "vco"),
          mod("vout", "voice_out"), mod("voices", "voices", {}, "global"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("lfo.out", "osc.fm"), cable("osc.out", "vout.left"),
          cable("voices.left", "out.left") })).warnings();
    CHECK_MSG(modulation.isEmpty(), modulation.join(" | "));
}

TEST(patch, warning_fixes)
{
    // Applies the fix of the first issue mentioning `text`; returns the fixed patch.
    auto fix = [](Patch p, const char* text) -> std::optional<Patch> {
        for (const auto& issue : p.issues())
            if (issue.text.contains(text)) {
                CHECK_MSG(!issue.fixOps.isEmpty() && !issue.fixLabel.isEmpty(), issue.text);
                const QString err = OpenDaw::Modular::applyOps(p, issue.fixOps);
                CHECK_MSG(err.isEmpty(), err);
                if (!err.isEmpty()) return std::nullopt;
                return p;
            }
        CHECK_MSG(false, std::string("no issue: ") + text);
        return std::nullopt;
    };
    auto feeder = [](const Patch& p, const char* module, const char* port) -> QString {
        for (const auto& c : p.cables)
            if (c.to.module == module && c.to.port == port) return c.from.toString();
        return {};
    };
    auto voicePatch = [](QJsonArray extraModules, QJsonArray extraCables, bool stereo) {
        QJsonArray modules{ mod("vin", "voice_in"), mod("osc", "vco"), mod("vout", "voice_out"),
                            mod("voices", "voices", {}, "global"), mod("out", "output") };
        QJsonArray cables{ cable("vin.pitch", "osc.pitch"), cable("osc.out", "vout.left"),
                           cable("voices.left", "out.left") };
        if (stereo) cables.append(cable("voices.right", "out.right"));
        for (const auto& m : extraModules) modules.append(m);
        for (const auto& c : extraCables) cables.append(c);
        return patchFrom(patchJson(modules, cables));
    };

    // Mono output already fed by voices: a new mixer sums voices and the drum.
    auto mono = fix(voicePatch({ mod("bd", "noise", {}, "global") }, { cable("bd.out", "vout.left") }, false),
                    "'bd' is in the global section");
    if (mono) {
        CHECK_MSG(mono->warnings().isEmpty(), mono->warnings().join(" | "));
        CHECK(feeder(*mono, "out", "left") == "mixer1.out");
        CHECK(feeder(*mono, "mixer1", "in1") == "voices.left");
        CHECK(feeder(*mono, "mixer1", "in2") == "bd.out");
        CHECK(feeder(*mono, "vout", "left") == "osc.out");     // the voice is untouched
        CHECK(mono->find("mixer1")->params.value("master") == 1.0f);   // no level drop
        CHECK(feeder(*mono, "out", "right").isEmpty());        // right still copies left
    }

    // Stereo output, drum into both voice_out sides: one issue, one fix for both sides.
    auto stereoIn = voicePatch({ mod("bd", "noise", {}, "global") },
                               { cable("bd.out", "vout.right") }, true);
    int bdIssues = 0;
    for (const auto& issue : stereoIn.issues()) bdIssues += issue.text.contains("'bd'");
    CHECK(bdIssues == 1);
    auto stereo = fix(stereoIn, "'bd' is in the global section");
    if (stereo) {
        CHECK_MSG(stereo->warnings().isEmpty(), stereo->warnings().join(" | "));
        CHECK(feeder(*stereo, "out", "left") == "mixer1.out" && feeder(*stereo, "out", "right") == "mixer2.out");
        CHECK(feeder(*stereo, "mixer2", "in1") == "voices.right" && feeder(*stereo, "mixer2", "in2") == "bd.out");
    }

    // A mixer already feeding the output: the drum takes its next free input.
    auto viaMixer = fix(patchFrom(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco"), mod("vout", "voice_out"),
          mod("voices", "voices", {}, "global"), mod("mix", "mixer", {}, "global"),
          mod("hat", "noise", {}, "global"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "vout.left"), cable("voices.left", "mix.in1"),
          cable("mix.out", "out.left"), cable("hat.out", "vout.left") })),
        "'hat' is in the global section");
    if (viaMixer) {
        CHECK(feeder(*viaMixer, "mix", "in2") == "hat.out");
        CHECK(viaMixer->find("mixer1") == nullptr);
        CHECK_MSG(viaMixer->warnings().isEmpty(), viaMixer->warnings().join(" | "));
    }

    // Output not fed while a voices module exists: connect it.
    auto unfed = fix(patchFrom(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco"), mod("vout", "voice_out"),
          mod("voices", "voices", {}, "global"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "vout.left") })),
        "left input has no cable");
    if (unfed) {
        CHECK(feeder(*unfed, "out", "left") == "voices.left" && feeder(*unfed, "out", "right") == "voices.right");
        CHECK_MSG(unfed->warnings().isEmpty(), unfed->warnings().join(" | "));
    }

    // voice_out with no voices module: add one and connect it.
    auto noVoices = fix(patchFrom(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco"), mod("vout", "voice_out"), mod("out", "output") },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "vout.left") })),
        "no global 'voices'");
    if (noVoices) {
        CHECK(feeder(*noVoices, "out", "left") == "voices.left");
        CHECK_MSG(noVoices->warnings().isEmpty(), noVoices->warnings().join(" | "));
    }

    // Warnings without a safe automatic fix carry none.
    for (const auto& issue : Patch{}.issues()) CHECK(issue.fixOps.isEmpty());
}
