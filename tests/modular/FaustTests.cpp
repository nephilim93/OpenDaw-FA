#include "TestHarness.h"
#include <algorithm>
#include <limits>

// Faust code modules. Tests that need the compiler return early when the
// build has no Faust support; the rest check the option-OFF behaviour too.

using namespace OpenDaw::Tests;
using OpenDaw::Modular::applyOps;
using OpenDaw::Modular::CompiledPatch;
using OpenDaw::Modular::Patch;

namespace {

const char* kGain = "import(\"stdfaust.lib\");\n"
                    "gain = hslider(\"gain\", 0.5, 0, 2, 0.01);\n"
                    "process = *(gain);\n";

const char* kOsc = "import(\"stdfaust.lib\");\n"
                   "freq = hslider(\"freq [unit:Hz]\", 440, 20, 20000, 0.01);\n"
                   "process = os.osc(freq);\n";

const char* kLowpass = "import(\"stdfaust.lib\");\n"
                       "cutoff = hslider(\"cutoff [unit:Hz]\", 1000, 20, 20000, 1);\n"
                       "process = fi.lowpass(2, cutoff);\n";

QJsonObject codeMod(const char* id, const char* code, const QJsonObject& params = {},
                    const char* section = "global")
{
    QJsonObject m = mod(id, "faust", params, section);
    m["code"] = code;
    return m;
}

QJsonObject output() { return mod("out", "output", { { "volume", 1 } }); }

QJsonObject writeOp(const char* id, const QString& code, const QJsonObject& params = {})
{
    QJsonObject op{ { "op", "write_module" }, { "id", id }, { "code", code },
                    { "section", "global" } };
    if (!params.isEmpty()) op["params"] = params;
    return op;
}

std::unique_ptr<CompiledPatch> compile(const QJsonObject& json)
{
    auto cp = CompiledPatch::build(patchFrom(json), kRate);
    CHECK(cp != nullptr);
    return cp;
}

// Frequency from interpolated upward zero crossings.
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

constexpr double kPi = 3.141592653589793;

double dB(double ratio) { return 20.0 * std::log10(ratio); }

} // namespace

TEST(faust, placeholder_is_registered)
{
    const auto* def = M::findModule("faust");
    CHECK(def != nullptr);
    if (!def) return;
    CHECK(def->inputs.empty() && def->outputs.empty() && def->params.empty());
    CHECK(def->allowedIn(M::Section::Voice) && def->allowedIn(M::Section::Global));
    CHECK(def->description.find("write_modular_module") != std::string::npos);

    // Code modules are written, not added like library modules.
    Patch p = Patch::initPatch();
    const QString err = applyOps(p, { QJsonObject{ { "op", "add_module" }, { "id", "f" },
                                                   { "type", "faust" } } });
    CHECK_MSG(err.contains("write_module"), err);
}

TEST(faust, loads_and_round_trips)
{
    // A saved patch with a code module loads with or without Faust: its
    // code, cables and parameter values survive a round trip.
    const auto json = patchJson(
        { mod("osc", "vco", { { "wave", "sine" }, { "level", 1 } }, "global"),
          codeMod("amp", kGain, { { "gain", 0.25 } }), output() },
        { cable("osc.out", "amp.in"), cable("amp.out", "out.left") });
    const Patch first = patchFrom(json);
    const auto* amp = first.find("amp");
    CHECK(amp != nullptr);
    if (!amp) return;
    CHECK(amp->code == kGain);
    CHECK(first.cables.size() == 2);
    CHECK_NEAR(amp->params.value("gain", -1.0f), 0.25, 1e-6);

    const QJsonObject saved = first.toJson();
    CHECK(saved["modules"].toArray()[1].toObject()["code"].toString() == kGain);
    const Patch second = patchFrom(saved);
    CHECK(second.toJson() == saved);

    auto cp = CompiledPatch::build(second, kRate);
    CHECK(cp != nullptr);
    if (!cp) return;
    const auto out = renderPatch(*cp, 4800);
    CHECK(allFinite(out.left));

    const QStringList warnings = second.warnings();
    const bool warned = std::any_of(warnings.begin(), warnings.end(), [](const QString& w) {
        return w.contains("'amp'") && w.contains("no Faust support");
    });
    if (M::codeModulesAvailable()) {
        CHECK_MSG(!warned, warnings.join(" | "));
        CHECK(amp->codeDef != nullptr);
        CHECK(rms(out.left, 0, 4800) > 0.1);
    } else {
        // Without Faust the code module is silent and says so.
        CHECK_MSG(warned, warnings.join(" | "));
        CHECK(rms(out.left, 0, 4800) == 0.0);
    }
}

TEST(faust, without_faust_writes_fail_cleanly)
{
    if (M::codeModulesAvailable()) return;
    Patch p = Patch::initPatch();
    const QJsonObject before = p.toJson();
    const QString err = applyOps(p, { writeOp("amp", kGain) });
    CHECK_MSG(err.contains("no Faust support") && err.contains("No changes were made"), err);
    CHECK(p.toJson() == before);
}

TEST(faust, limits)
{
    // At most kMaxCodeModules code modules per patch.
    QJsonArray modules;
    for (int i = 0; i <= M::kMaxCodeModules; ++i)
        modules.append(codeMod(qPrintable(QString("f%1").arg(i)), kGain));
    QString error;
    CHECK(!Patch::fromJson(patchJson(modules, {}), &error).has_value());
    CHECK_MSG(error.contains("limit is 8"), error);
    modules.removeLast();
    CHECK_MSG(Patch::fromJson(patchJson(modules, {}), &error).has_value(), error);

    // And at most kMaxCodeSize bytes of source.
    const QString huge = QString(kGain) + "// " + QString(M::kMaxCodeSize, 'x') + "\n";
    CHECK(!Patch::fromJson(patchJson({ codeMod("big", qPrintable(huge)) }, {}), &error).has_value());
    CHECK_MSG(error.contains("bytes of code") && error.contains("65536"), error);

    // Empty code is an error even when it can't be compiled.
    CHECK(!Patch::fromJson(patchJson({ codeMod("empty", "  ") }, {}), &error).has_value());
    CHECK_MSG(error.contains("no Faust code"), error);
}

TEST(faust, gain_ports_and_render)
{
    if (!M::codeModulesAvailable()) return;
    const auto result = M::compileCodeModule("amp", kGain);
    CHECK_MSG(result.def != nullptr, result.error);
    if (!result.def) return;
    const auto& def = *result.def;
    CHECK(def.type == "faust");
    CHECK(def.inputs.size() == 1 && def.inputs[0].name == "in");
    CHECK(def.outputs.size() == 1 && def.outputs[0].name == "out");
    CHECK(def.params.size() == 1);
    if (def.params.size() != 1) return;
    CHECK(def.params[0].name == "gain");
    CHECK(def.params[0].min == 0.0f && def.params[0].max == 2.0f && def.params[0].defaultValue == 0.5f);

    // Left: the sine through the code module at gain 0.25; right: the sine.
    auto cp = compile(patchJson(
        { mod("osc", "vco", { { "wave", "sine" }, { "level", 1 } }, "global"),
          codeMod("amp", kGain, { { "gain", 0.25 } }), output() },
        { cable("osc.out", "amp.in"), cable("amp.out", "out.left"), cable("osc.out", "out.right") }));
    if (!cp) return;
    const auto out = renderPatch(*cp, 4800);
    // One float multiply: a couple of float ulps at full scale.
    double worst = 0.0;
    for (size_t i = 0; i < out.left.size(); ++i)
        worst = std::max(worst, std::abs(double(out.left[i]) - 0.25 * out.right[i]));
    CHECK_MSG(worst <= 5e-7, "max error " + str(worst));
    CHECK(rms(out.right, 0, 4800) > 0.5);
}

TEST(faust, oscillator_pitch)
{
    if (!M::codeModulesAvailable()) return;
    auto cp = compile(patchJson({ codeMod("osc", kOsc, { { "freq", 330 } }), output() },
                                { cable("osc.out", "out.left") }));
    if (!cp) return;

    const auto def = M::compileCodeModule("osc", kOsc).def;
    CHECK(def && def->inputs.empty() && def->outputs.size() == 1);
    CHECK(def && def->params.size() == 1 && def->params[0].unit == "Hz");

    // Interpolated zero crossings over a second resolve far below a cent;
    // 0.1 cent is well inside anything audible.
    const auto out = renderPatch(*cp, int(kRate));
    const double hz = measureHz(out.left, 0, int(kRate));
    const double cents = 1200.0 * std::log2(hz / 330.0);
    CHECK_MSG(std::abs(cents) <= 0.1, str(hz) + " Hz, " + str(cents) + " cents");
    CHECK_NEAR(*std::max_element(out.left.begin(), out.left.end()), 1.0, 1e-3);
}

TEST(faust, filter_response)
{
    if (!M::codeModulesAvailable()) return;
    // A Faust oscillator into a Faust lowpass: left = filtered, right = dry.
    // fi.lowpass(2, fc) is a bilinear Butterworth prewarped at fc, so its gain is
    // 1 / sqrt(1 + (tan(pi f / fs) / tan(pi fc / fs))^4).
    const double fc = 2000.0;
    for (const double f : { 200.0, 2000.0, 6000.0 }) {
        auto cp = compile(patchJson(
            { codeMod("osc", kOsc, { { "freq", f } }), codeMod("lp", kLowpass, { { "cutoff", fc } }),
              output() },
            { cable("osc.out", "lp.in"), cable("lp.out", "out.left"), cable("osc.out", "out.right") }));
        if (!cp) return;
        const auto out = renderPatch(*cp, int(kRate));
        // After 0.1 s of settling, a whole number of periods.
        const int from = int(kRate * 0.1);
        const int to = from + int(std::round(std::floor(0.8 * f) * kRate / f));
        const double measured = dB(rms(out.left, from, to) / rms(out.right, from, to));
        const double ratio = std::tan(kPi * f / kRate) / std::tan(kPi * fc / kRate);
        const double expected = dB(1.0 / std::sqrt(1.0 + std::pow(ratio, 4.0)));
        // The RMS ratio is good to ~0.001 dB here; 0.05 dB is inaudible.
        CHECK_MSG(std::abs(measured - expected) <= 0.05,
                  str(f) + " Hz: " + str(measured) + " dB, expected " + str(expected));
    }
}

TEST(faust, voice_section)
{
    if (!M::codeModulesAvailable()) return;
    // One DSP instance per voice: two notes through a per-voice code module.
    auto cp = compile(patchJson(
        { mod("vin", "voice_in"), mod("osc", "vco", { { "wave", "sine" }, { "level", 1 } }),
          codeMod("amp", kGain, { { "gain", 0.5 } }, "voice"), mod("vout", "voice_out"),
          mod("voices", "voices"), output() },
        { cable("vin.pitch", "osc.pitch"), cable("osc.out", "amp.in"), cable("amp.out", "vout.left"),
          cable("voices.left", "out.left") }));
    if (!cp) return;
    cp->noteOn(60, 1.0f);
    const auto one = renderPatch(*cp, 4800);
    CHECK_NEAR(*std::max_element(one.left.begin(), one.left.end()), 0.5, 0.01);
    cp->noteOn(72, 1.0f);
    const auto two = renderPatch(*cp, 4800);
    CHECK(allFinite(two.left));
    CHECK(*std::max_element(two.left.begin(), two.left.end()) > 0.6f);
}

TEST(faust, nan_guard)
{
    if (!M::codeModulesAvailable()) return;
    // log of a negative control value is NaN at runtime; macro 1 sweeps x.
    const char* code = "x = hslider(\"x\", 1, -2, 2, 0.01);\nprocess = log(x);\n";
    auto json = patchJson({ codeMod("bad", code), output() }, { cable("bad.out", "out.left") });
    json["macros"] = QJsonArray{ QJsonObject{
        { "name", "X" },
        { "targets", QJsonArray{ QJsonObject{ { "module", "bad" }, { "param", "x" },
                                              { "min", -1 }, { "max", 2 } } } } } };
    auto cp = compile(json);
    if (!cp) return;

    std::vector<float> l(4800, 0.0f), r(4800, 0.0f);
    float macros[M::kNumMacros] = {};
    cp->render(l.data(), r.data(), int(l.size()), macros);
    CHECK(allFinite(l));
    CHECK(*std::max_element(l.begin(), l.end()) == 0.0f);

    // Recovers once the value is sane.
    macros[0] = 1.0f;
    std::fill(l.begin(), l.end(), 0.0f);
    cp->render(l.data(), r.data(), int(l.size()), macros);
    CHECK_NEAR(l.back(), std::log(2.0), 1e-6);
}

TEST(faust, compile_errors_are_reported)
{
    if (!M::codeModulesAvailable()) return;
    Patch p = Patch::initPatch();
    const QJsonObject before = p.toJson();

    // A syntax error, with its line.
    QString err = applyOps(p, { writeOp("fx", "import(\"stdfaust.lib\");\nprocess = *(0.5) +;\n") });
    CHECK_MSG(err.contains("Faust code for 'fx' doesn't compile: line 2:") && err.contains("syntax error")
                  && err.contains("No changes were made"), err);
    CHECK(p.toJson() == before);

    // An undefined name.
    err = applyOps(p, { writeOp("fx", "import(\"stdfaust.lib\");\n\nprocess = wobble;\n") });
    CHECK_MSG(err.contains("line 3: undefined symbol : wobble"), err);

    // Native calls are refused before compiling.
    err = applyOps(p, { writeOp("fx", "f = ffunction(float sinf(float), <math.h>, \"\");\nprocess = f;\n") });
    CHECK_MSG(err.contains("line 1: 'ffunction' is not allowed"), err);

    // Code that compiles but makes nothing.
    err = applyOps(p, { writeOp("fx", "process = !;\n") });
    CHECK_MSG(err.contains("no output"), err);

    // A saved patch whose code no longer compiles is a validation error.
    QString error;
    CHECK(!Patch::fromJson(patchJson({ codeMod("fx", "process = +(;\n") }, {}), &error).has_value());
    CHECK_MSG(error.contains("Code module 'fx' doesn't compile: line 1:"), error);

    // A compile that runs past its deadline is abandoned.
    const auto slow = M::compileCodeModule("slow", "process = _ * 0.123456;\n", 0.0);
    CHECK(slow.def == nullptr);
    CHECK_MSG(slow.error.find("longer than") != std::string::npos, slow.error);
    CHECK(p.toJson() == before);
}

TEST(faust, write_module_adds_and_updates)
{
    if (!M::codeModulesAvailable()) return;
    Patch p = patchFrom(patchJson(
        { mod("osc", "vco", { { "wave", "sine" } }, "global"), output() }, {}));

    // Add, wired in and on a macro, in one batch.
    QString err = applyOps(p, {
        writeOp("amp", kGain, { { "gain", 1.5 } }),
        QJsonObject{ { "op", "connect" }, { "from", "osc.out" }, { "to", "amp.in" } },
        QJsonObject{ { "op", "connect" }, { "from", "amp.out" }, { "to", "out.left" } },
        QJsonObject{ { "op", "set_macro" }, { "index", 0 }, { "name", "Gain" },
                     { "targets", QJsonArray{ QJsonObject{ { "module", "amp" }, { "param", "gain" } } } } },
    });
    CHECK_MSG(err.isEmpty(), err);
    const auto* amp = p.find("amp");
    CHECK(amp && amp->section == M::Section::Global && amp->params.value("gain") == 1.5f);
    CHECK(p.cables.size() == 2);

    // Ports and parameters are checked against the compiled code.
    err = applyOps(p, { QJsonObject{ { "op", "connect" }, { "from", "amp.out2" }, { "to", "out.right" } } });
    CHECK_MSG(err.contains("no output 'out2'") && err.contains("Outputs: out"), err);
    err = applyOps(p, { QJsonObject{ { "op", "set_param" }, { "module", "amp" }, { "param", "gain" },
                                     { "value", 3 } } });
    CHECK_MSG(err.contains("out of range"), err);

    // New code: the input still fits, the stereo outputs and new control don't
    // match the old cable and macro, which are dropped.
    const char* stereo = "level = hslider(\"level\", 1, 0, 1, 0.01);\n"
                         "process = *(level) <: _, _;\n";
    err = applyOps(p, { writeOp("amp", stereo) });
    CHECK_MSG(err.isEmpty(), err);
    amp = p.find("amp");
    CHECK(amp && amp->code == stereo && amp->params.isEmpty());
    CHECK(p.cables.size() == 1 && p.cables[0].to.toString() == "amp.in");
    CHECK(p.macros[0].targets.isEmpty());
    CHECK(amp && amp->codeDef && amp->codeDef->outputs.size() == 2
          && amp->codeDef->outputs[1].name == "out2");

    // Ids of library modules can't be overwritten.
    err = applyOps(p, { writeOp("osc", kGain) });
    CHECK_MSG(err.contains("'osc' is a vco module"), err);
}
