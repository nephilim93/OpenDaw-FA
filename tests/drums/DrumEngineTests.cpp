#include "../modular/TestHarness.h"
#include "drums/DrumEngine.h"
#include "drums/DrumKit.h"
#include "drums/KitFromFolder.h"
#include "drums/KitPresets.h"
#include "drums/SampleLibrary.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

// The Drum Machine's kit model and engine.

using namespace OpenDaw::Tests;
namespace D = OpenDaw::Drums;

namespace {

D::Kit oneSynthPad(const char* module, int note = 36, float level = 1.0f, float pan = 0.0f)
{
    D::Kit kit;
    D::Pad p;
    p.note = note;
    p.name = "pad";
    p.module = module;
    p.level = level;
    p.pan = pan;
    kit.pads = { p };
    return kit;
}

struct Out {
    std::vector<float> left, right;
};

// Renders `len` samples, hitting `hits` (sample, note, velocity) on the way,
// in blocks of `block` as a host would.
Out render(D::DrumEngine& e, int len, std::vector<std::tuple<int, int, float>> hits, int block = 256)
{
    Out o{ std::vector<float>(size_t(len)), std::vector<float>(size_t(len)) };
    std::sort(hits.begin(), hits.end());
    size_t h = 0;
    int pos = 0;
    while (pos < len) {
        int end = std::min(len, pos + block);
        if (h < hits.size() && std::get<0>(hits[h]) < end) end = std::max(pos, std::get<0>(hits[h]));
        if (end > pos) e.render(o.left.data() + pos, o.right.data() + pos, end - pos);
        pos = end;
        while (h < hits.size() && std::get<0>(hits[h]) <= pos) {
            e.noteOn(std::get<1>(hits[h]), std::get<2>(hits[h]));
            ++h;
        }
    }
    return o;
}

std::vector<float> triggerAt0(int len)
{
    std::vector<float> t(size_t(len), 0.0f);
    const int n = int(std::lround(D::DrumEngine::kTriggerSeconds * kRate));
    std::fill(t.begin(), t.begin() + n, 1.0f);
    return t;
}

double maxAbs(const std::vector<float>& s, int from = 0, int to = -1)
{
    if (to < 0) to = int(s.size());
    double m = 0.0;
    for (int i = from; i < to; ++i) m = std::max(m, double(std::abs(s[size_t(i)])));
    return m;
}

} // namespace

TEST(drums, default_kit_is_valid_and_round_trips)
{
    const auto kit = D::Kit::defaultKit();
    CHECK_MSG(kit.validate().isEmpty(), kit.validate().join(" | "));
    for (int note : { D::Gm::Kick, D::Gm::Snare, D::Gm::ClosedHat, D::Gm::OpenHat, D::Gm::Crash, D::Gm::Ride })
        CHECK_MSG(kit.padForNote(note) != nullptr, "no pad for " + str(note));
    // Hats choke each other; nothing else is in their group.
    const int group = kit.padForNote(D::Gm::OpenHat)->choke;
    CHECK(group != 0 && kit.padForNote(D::Gm::ClosedHat)->choke == group
          && kit.padForNote(D::Gm::PedalHat)->choke == group);
    CHECK(kit.padForNote(D::Gm::Kick)->choke == 0);
    // Each GM sound uses its own voice, not a stand-in.
    CHECK(kit.padForNote(D::Gm::Clap)->module == "clap");
    CHECK(kit.padForNote(D::Gm::SideStick)->module == "rimshot");
    CHECK(kit.padForNote(D::Gm::Cowbell)->module == "cowbell");
    CHECK(kit.padForNote(D::Gm::Crash)->module == "cymbal" && kit.padForNote(D::Gm::Ride)->module == "cymbal");
    for (int note : { D::Gm::LowFloorTom, D::Gm::HighFloorTom, D::Gm::LowTom, D::Gm::LowMidTom,
                      D::Gm::HighMidTom, D::Gm::HighTom })
        CHECK_MSG(kit.padForNote(note)->module == "tom", "tom pad " + str(note));
    // Toms rise in pitch with the note number.
    float lastTune = 0.0f;
    for (int note : { D::Gm::LowFloorTom, D::Gm::HighFloorTom, D::Gm::LowTom, D::Gm::LowMidTom,
                      D::Gm::HighMidTom, D::Gm::HighTom }) {
        const float tune = kit.padForNote(note)->params.value("tune");
        CHECK_MSG(tune > lastTune, "tom " + str(note) + " at " + str(tune));
        lastTune = tune;
    }

    QString err;
    const auto back = D::Kit::fromJson(kit.toJson(), &err);
    CHECK_MSG(back.has_value(), err);
    if (back) CHECK(back->toJson() == kit.toJson());
}

TEST(drums, invalid_kits_are_explained)
{
    auto fails = [](QJsonObject json, const char* text) {
        QString err;
        const auto kit = D::Kit::fromJson(json, &err);
        CHECK_MSG(!kit && err.contains(text), "expected '" + std::string(text) + "', got: " + err.toStdString());
    };
    auto pads = [](std::initializer_list<QJsonObject> list) {
        QJsonArray a;
        for (const auto& o : list) a.append(o);
        return QJsonObject{ { "pads", a } };
    };
    fails(pads({ QJsonObject{ { "note", 36 }, { "module", "kick" } }, QJsonObject{ { "note", 36 }, { "module", "snare" } } }),
          "Two pads play note 36");
    fails(pads({ QJsonObject{ { "note", 36 }, { "module", "vco" } } }), "can't be a synth pad. Use one of: ");
    fails(pads({ QJsonObject{ { "note", 36 }, { "module", "kick" }, { "params", QJsonObject{ { "boom", 1 } } } } }),
          "has no parameter 'boom'");
    fails(pads({ QJsonObject{ { "note", 36 }, { "module", "kick" }, { "params", QJsonObject{ { "tune", 9999 } } } } }),
          "out of range");
    fails(pads({ QJsonObject{ { "note", 36 }, { "source", "tape" } } }), "source must be");
    fails(pads({ QJsonObject{ { "note", 36 }, { "source", "sample" } } }), "without a sample file");
    fails(pads({ QJsonObject{ { "note", 200 }, { "module", "kick" } } }), "outside MIDI");
    // Pads take drum voices only, not other modules that happen to have a trigger input.
    const QStringList voices = D::synthPadModules();
    for (const char* drum : { "kick", "snare", "hihat", "clap", "tom", "cymbal", "rimshot", "cowbell" })
        CHECK_MSG(voices.contains(drum), drum);
    for (const char* other : { "vco", "sample_hold", "random", "fm_voice" })
        CHECK_MSG(!voices.contains(other), other);
    fails(pads({ QJsonObject{ { "note", 36 }, { "module", "sample_hold" } } }), "can't be a synth pad");
}

TEST(drums, synth_pad_matches_its_module)
{
    // A centred pad at level 1 plays exactly what the module makes for a
    // 1 ms trigger with the hit's velocity as accent.
    const int len = 9600;
    auto engine = D::DrumEngine::build(oneSynthPad("kick"), kRate, {});
    const auto out = render(*engine, len, { { 0, 36, 0.7f } });
    ModuleRunner direct("kick");
    const auto want = direct.run(len, { { "trigger", triggerAt0(len) },
                                        { "accent", std::vector<float>(size_t(len), 0.7f) },
                                        { "pitch", std::vector<float>(size_t(len), 0.0f) } })[0];
    double worst = 0.0;
    for (int i = 0; i < len; ++i)
        worst = std::max({ worst, std::abs(double(out.left[size_t(i)]) - want[size_t(i)]),
                           std::abs(double(out.right[size_t(i)]) - want[size_t(i)]) });
    CHECK_MSG(worst == 0.0, "max difference " + str(worst));
    CHECK(maxAbs(out.left) > 0.3);

    // Unmapped notes and silence before the hit.
    auto quiet = D::DrumEngine::build(oneSynthPad("kick"), kRate, {});
    CHECK(maxAbs(render(*quiet, 4800, { { 0, 40, 1.0f } }).left) == 0.0);
}

TEST(drums, level_and_pan)
{
    const int len = 4800;
    auto ref = D::DrumEngine::build(oneSynthPad("snare", 38, 1.0f, 0.0f), kRate, {});
    auto left = D::DrumEngine::build(oneSynthPad("snare", 38, 0.5f, -1.0f), kRate, {});
    auto halfRight = D::DrumEngine::build(oneSynthPad("snare", 38, 1.0f, 0.5f), kRate, {});
    const auto r = render(*ref, len, { { 0, 38, 1.0f } });
    const auto l = render(*left, len, { { 0, 38, 1.0f } });
    const auto h = render(*halfRight, len, { { 0, 38, 1.0f } });
    double err = 0.0;
    for (size_t i = 0; i < size_t(len); ++i)
        err = std::max({ err, std::abs(l.left[i] - 0.5 * r.left[i]), std::abs(double(l.right[i])),
                         std::abs(h.left[i] - 0.5 * r.left[i]), std::abs(double(h.right[i]) - r.right[i]) });
    // One float multiply each: a few ulps at most.
    CHECK_MSG(err < 1e-6, "level/pan error " + str(err));
}

TEST(drums, tune_shifts_synth_pitch)
{
    // The kick settles to its tune; +12 semitones doubles it.
    auto kit = oneSynthPad("kick");
    kit.pads[0].params = { { "tune", 50.0f }, { "decay", 2.0f }, { "click", 0.0f }, { "drive", 0.0f } };
    kit.pads[0].tune = 12.0f;
    auto engine = D::DrumEngine::build(kit, kRate, {});
    const auto out = render(*engine, int(kRate * 0.6), { { 0, 36, 1.0f } });
    int crossings = 0;
    double first = -1, last = -1;
    for (int i = int(kRate * 0.35) + 1; i < int(kRate * 0.6); ++i)
        if (out.left[size_t(i - 1)] < 0.0f && out.left[size_t(i)] >= 0.0f) {
            const double t = (i - 1) + out.left[size_t(i - 1)] / double(out.left[size_t(i - 1)] - out.left[size_t(i)]);
            if (first < 0) first = t;
            last = t;
            ++crossings;
        }
    const double hz = crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
    // Same bound as the kick module's own test (0.2%).
    CHECK_MSG(std::abs(hz / 100.0 - 1.0) < 0.002, "tuned kick at " + str(hz) + " Hz");
}

TEST(drums, choke_cuts_the_open_hat)
{
    // Open hat alone, vs. open hat then a closed hat 0.1 s later: after the
    // choke fade the mix is the closed hat alone.
    auto kit = D::Kit::defaultKit();
    const int len = int(kRate * 0.4), hitAt = int(kRate * 0.1);
    const int fade = int(std::ceil(D::DrumEngine::kChokeSeconds * kRate));
    auto both = D::DrumEngine::build(kit, kRate, {});
    auto closedOnly = D::DrumEngine::build(kit, kRate, {});
    const auto a = render(*both, len, { { 0, D::Gm::OpenHat, 1.0f }, { hitAt, D::Gm::ClosedHat, 1.0f } });
    const auto b = render(*closedOnly, len, { { hitAt, D::Gm::ClosedHat, 1.0f } });
    CHECK(maxAbs(a.left, hitAt - 2000, hitAt) > 0.05);   // the open hat was ringing
    double after = 0.0;
    for (int i = hitAt + fade + 1; i < len; ++i)
        after = std::max(after, std::abs(double(a.left[size_t(i)]) - b.left[size_t(i)]));
    CHECK_MSG(after < 1e-6, "open hat left after the choke: " + str(after));
    // The fade itself is gradual: no step larger than the hat's own motion.
    double jump = 0.0;
    for (int i = hitAt; i < hitAt + fade; ++i)
        jump = std::max(jump, std::abs(double(a.left[size_t(i + 1)] - b.left[size_t(i + 1)])
                                      - (a.left[size_t(i)] - b.left[size_t(i)])));
    CHECK_MSG(jump < 0.5, "choke step " + str(jump));

    // Pads outside the group are untouched: a kick under the choke is identical.
    auto withKick = D::DrumEngine::build(kit, kRate, {});
    auto kickOnly = D::DrumEngine::build(kit, kRate, {});
    const auto k1 = render(*withKick, len, { { 0, D::Gm::Kick, 1.0f }, { hitAt, D::Gm::ClosedHat, 1.0f } });
    const auto k2 = render(*kickOnly, len, { { 0, D::Gm::Kick, 1.0f } });
    const auto hatOnly = render(*D::DrumEngine::build(kit, kRate, {}), len, { { hitAt, D::Gm::ClosedHat, 1.0f } });
    double kickErr = 0.0;
    for (size_t i = 0; i < size_t(len); ++i)
        kickErr = std::max(kickErr, std::abs(double(k1.left[i]) - k2.left[i] - hatOnly.left[i]));
    CHECK_MSG(kickErr < 1e-6, "kick changed by the choke: " + str(kickErr));
}

TEST(drums, sample_pads)
{
    // A ramp sample at half the engine rate: tune 0 plays it at its own speed
    // (twice as many output samples), tune +12 at the engine rate.
    auto ramp = std::make_shared<D::SampleData>();
    ramp->sampleRate = kRate / 2;
    for (int i = 0; i < 1000; ++i) ramp->left.push_back(float(i) / 1000.0f);
    D::SampleMap samples{ { "ramp.wav", ramp } };

    D::Kit kit;
    D::Pad p;
    p.note = 40;
    p.source = D::PadSource::Sample;
    p.sample = "ramp.wav";
    p.level = 1.0f;
    kit.pads = { p };
    auto engine = D::DrumEngine::build(kit, kRate, samples);
    const auto out = render(*engine, 4000, { { 0, 40, 0.5f } });
    // Output i is the ramp at position i/2: 0.5 * (i/2)/1000, linearly interpolated (exact for a ramp).
    double err = 0.0;
    for (int i = 0; i < 1998; ++i) err = std::max(err, std::abs(out.left[size_t(i)] - 0.5 * (i / 2.0) / 1000.0));
    CHECK_MSG(err < 1e-6, "playback error " + str(err));
    CHECK(maxAbs(out.left, 2000, 4000) == 0.0);   // ended

    kit.pads[0].tune = 12.0f;
    auto up = D::DrumEngine::build(kit, kRate, samples);
    const auto fast = render(*up, 2000, { { 0, 40, 1.0f } });
    CHECK(std::abs(fast.left[500] - 0.5f) < 1e-6);
    CHECK(maxAbs(fast.left, 1000, 2000) == 0.0);

    // A retrigger fades the old playhead out instead of cutting it.
    kit.pads[0].tune = 0.0f;
    auto retrig = D::DrumEngine::build(kit, kRate, samples);
    const auto r = render(*retrig, 4000, { { 0, 40, 1.0f }, { 1000, 40, 1.0f } });
    double jump = 0.0;
    for (int i = 990; i < 1200; ++i) jump = std::max(jump, double(std::abs(r.left[size_t(i + 1)] - r.left[size_t(i)])));
    // The ramp moves 0.0005 per output sample; the 3 ms fade of a 0.5 level
    // over ~132 samples adds at most ~0.004 per sample.
    CHECK_MSG(jump < 0.01, "retrigger step " + str(jump));

    // A missing sample is reported and silent.
    QStringList warnings;
    kit.pads[0].sample = "missing.wav";
    auto silent = D::DrumEngine::build(kit, kRate, samples, &warnings);
    CHECK(warnings.size() == 1 && warnings[0].contains("missing.wav"));
    CHECK(maxAbs(render(*silent, 1000, { { 0, 40, 1.0f } }).left) == 0.0);
}

TEST(drums, whole_kit_is_finite_and_quiet_pads_idle)
{
    // Every default pad hit at full velocity, twice, then silence.
    auto engine = D::DrumEngine::build(D::Kit::defaultKit(), kRate, {});
    std::vector<std::tuple<int, int, float>> hits;
    for (const auto& pad : D::Kit::defaultKit().pads) {
        hits.push_back({ 100, pad.note, 1.0f });
        hits.push_back({ 101, pad.note, 1.0f });   // retrigger one sample later
    }
    const int len = int(kRate * 6);
    const auto out = render(*engine, len, hits);
    CHECK(allFinite(out.left) && allFinite(out.right));
    CHECK(maxAbs(out.left, 0, 100) == 0.0);
    CHECK(maxAbs(out.left, 100, 4000) > 0.3);
    // Longest pad decay is 2.5 s at -60 dB; by 5 s everything is far below that.
    CHECK(maxAbs(out.left, int(kRate * 5), len) < 1e-4);
}

// ── Samples folder: kit paths ───────────────────────────────────────────────

TEST(drums, sample_paths_inside_the_folder_are_relative)
{
    QTemporaryDir root;
    CHECK(root.isValid());
    const QString folder = root.path() + "/Samples";
    QDir().mkpath(folder + "/808");
    QDir().mkpath(root.path() + "/Samples2");
    const QString kick = folder + "/808/kick.wav";
    QFile(kick).open(QIODevice::WriteOnly);

    CHECK(D::toKitPath(kick, folder) == "808/kick.wav");
    CHECK(D::toKitPath(folder + "/808/../808/kick.wav", folder) == "808/kick.wav");
    // A trailing slash on the folder changes nothing.
    CHECK(D::toKitPath(kick, folder + "/") == "808/kick.wav");
    // A file that doesn't exist (yet) still maps by its path.
    CHECK(D::toKitPath(folder + "/new/snare.wav", folder) == "new/snare.wav");
    // Outside the folder (including a sibling whose name starts the same): absolute.
    const QString outside = root.path() + "/Samples2/clap.wav";
    CHECK(QDir::isAbsolutePath(D::toKitPath(outside, folder)));
    CHECK(D::toKitPath(outside, folder).endsWith("/Samples2/clap.wav"));
    CHECK(QDir::isAbsolutePath(D::toKitPath(root.path() + "/hat.wav", folder)));
    // Already relative, or no folder: unchanged.
    CHECK(D::toKitPath("808/kick.wav", folder) == "808/kick.wav");
    CHECK(D::toKitPath(kick, QString()) == kick);
}

TEST(drums, kit_paths_resolve_against_the_folder)
{
    CHECK(D::resolveSamplePath("808/kick.wav", "/music/Samples") == "/music/Samples/808/kick.wav");
    CHECK(D::resolveSamplePath("/elsewhere/kick.wav", "/music/Samples") == "/elsewhere/kick.wav");
    CHECK(D::resolveSamplePath("", "/music/Samples").isEmpty());
    // Moving the folder moves every relative sample with it.
    CHECK(D::resolveSamplePath("808/kick.wav", "/new/place") == "/new/place/808/kick.wav");

    QTemporaryDir root;
    const QString folder = root.path() + "/Samples";
    QDir().mkpath(folder + "/Acoustic");
    const QString snare = folder + "/Acoustic/snare.wav";
    QFile(snare).open(QIODevice::WriteOnly);
    CHECK(QFileInfo(D::resolveSamplePath(D::toKitPath(snare, folder), folder)).canonicalFilePath()
          == QFileInfo(snare).canonicalFilePath());
}

// ── Kit presets ─────────────────────────────────────────────────────────────

TEST(drums, built_in_presets_are_valid_and_distinct)
{
    QTemporaryDir root;
    const auto presets = D::listKitPresets(root.path());
    CHECK(presets.size() == 3);
    QSet<QString> kicks;
    for (const auto& p : presets) {
        CHECK(p.builtIn);
        QString error;
        const auto kit = D::loadKitPreset(p.name, root.path(), &error);
        CHECK(kit.has_value());
        if (!kit) continue;
        CHECK(kit->validate().isEmpty());
        CHECK(kit->padForNote(D::Gm::Kick) != nullptr);
        const auto params = kit->padForNote(D::Gm::Kick)->params;
        kicks.insert(QString::number(params.value("tune")) + "/" + QString::number(params.value("decay")));
    }
    CHECK(kicks.size() == 3);   // each preset has its own kick
    QString error;
    CHECK(D::loadKitPreset("synth 808", root.path(), &error).has_value());   // case doesn't matter
    CHECK(!D::loadKitPreset("Nope", root.path(), &error).has_value());
    CHECK(error.contains("Synth 909"));   // the message lists the presets
}

TEST(drums, user_presets_save_and_load_with_relative_samples)
{
    QTemporaryDir root;
    const QString samples = root.path() + "/Samples";
    QDir().mkpath(samples + "/Acoustic");
    QFile(samples + "/Acoustic/kick.wav").open(QIODevice::WriteOnly);

    auto kit = D::Kit::defaultKit();
    auto* kick = kit.padForNote(D::Gm::Kick);
    kick->source = D::PadSource::Sample;
    kick->module.clear();
    kick->params.clear();
    kick->sample = samples + "/Acoustic/kick.wav";   // absolute, inside the folder
    auto* clap = kit.padForNote(D::Gm::Clap);
    clap->source = D::PadSource::Sample;
    clap->module.clear();
    clap->params.clear();
    clap->sample = root.path() + "/elsewhere/clap.wav";   // outside it

    QString error;
    QStringList outside;
    CHECK(D::saveKitPreset(kit, "  My Kit ", samples, false, &error, &outside));
    CHECK(QFileInfo::exists(samples + "/Kits/My Kit.drumkit"));
    CHECK(outside.size() == 1 && outside[0].contains("clap.wav"));

    const auto presets = D::listKitPresets(samples);
    CHECK(presets.size() == 4 && presets.last().name == "My Kit" && !presets.last().builtIn);
    const auto loaded = D::loadKitPreset("my kit", samples, &error);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->padForNote(D::Gm::Kick)->sample == "Acoustic/kick.wav");
        CHECK(QDir::isAbsolutePath(loaded->padForNote(D::Gm::Clap)->sample));
        CHECK(loaded->pads.size() == kit.pads.size());
        CHECK(loaded->padForNote(D::Gm::Snare)->module == "snare");
    }

    // Existing names need overwrite; built-in names and bad names are refused.
    CHECK(!D::saveKitPreset(kit, "my kit", samples, false, &error));
    CHECK(error.contains("already exists"));
    CHECK(D::saveKitPreset(D::Kit::defaultKit(), "My Kit", samples, true, &error));
    CHECK(D::loadKitPreset("My Kit", samples, &error)->padForNote(D::Gm::Kick)->source == D::PadSource::Synth);
    CHECK(!D::saveKitPreset(kit, "Synth 808", samples, true, &error));
    CHECK(error.contains("built-in"));
    for (const char* bad : { "", "   ", "a/b", "..\\up", ".hidden", "x:y" })
        CHECK(!D::saveKitPreset(kit, bad, samples, true, &error));
    CHECK(D::listKitPresets(samples).size() == 4);

    // A broken file is reported, not loaded.
    QFile broken(samples + "/Kits/Broken.drumkit");
    broken.open(QIODevice::WriteOnly);
    broken.write("{ not json");
    broken.close();
    CHECK(!D::loadKitPreset("Broken", samples, &error).has_value());
    CHECK(error.contains("Broken.drumkit"));
}

// ── Kits from sample folders ────────────────────────────────────────────────

TEST(drums, sample_names_map_to_gm_notes)
{
    const std::vector<std::pair<const char*, int>> cases = {
        { "Kick.wav", 36 }, { "BD_Punchy01.wav", 36 }, { "808 Kick Long.wav", 36 }, { "BassDrum.aif", 36 },
        { "Kicks/001.wav", 36 }, { "kik_hard.wav", 36 },
        { "Snare.wav", 38 }, { "SD Tight.wav", 38 }, { "Snares/Fat 2.wav", 38 },
        { "Clap.wav", 39 }, { "CP_909.wav", 39 }, { "Rimshot.wav", 37 }, { "Side Stick.wav", 37 },
        { "HH Closed.wav", 42 }, { "HiHat.wav", 42 }, { "Closed Hat 2.wav", 42 }, { "CH.wav", 42 },
        { "OpenHat.wav", 46 }, { "OH_1.wav", 46 }, { "Hats/Open.wav", 46 }, { "Hi-Hat Open.flac", 46 },
        { "Pedal Hat.wav", 44 }, { "HH Foot.wav", 44 },
        { "Tom Floor.wav", 41 }, { "Floor Tom High.wav", 43 }, { "Tom Low.wav", 45 }, { "Mid Tom.wav", 47 },
        { "Hi Tom.wav", 50 }, { "Toms/High.wav", 50 }, { "LT.wav", 45 },
        { "Crash 18.wav", 49 }, { "Ride.wav", 51 }, { "Ride Bell.wav", 53 }, { "Splash.wav", 55 },
        { "China.wav", 52 }, { "Cymbals/Crash.wav", 49 },
        { "Cowbell.wav", 56 }, { "Tambourine.wav", 54 }, { "Shaker.wav", 70 }, { "Clave.wav", 75 },
        // Folder words decide only when the file name says nothing.
        { "Kicks/Snare Layer.wav", 38 },
    };
    for (const auto& [path, note] : cases) {
        const auto guess = D::guessDrumNote(path);
        if (!guess || *guess != note)
            reportFailure(__FILE__, __LINE__, std::string(path) + " -> " + (guess ? std::to_string(*guess) : "none")
                                                  + ", expected " + std::to_string(note));
    }
    for (const char* none : { "Custom FX.wav", "Bottom End.wav", "That Sound.wav", "Trim 3.wav", "Loop 120bpm.wav",
                              "Pad.wav", "808 Bass.wav" })
        if (const auto guess = D::guessDrumNote(none))
            reportFailure(__FILE__, __LINE__, std::string(none) + " should not match, got " + std::to_string(*guess));
}

TEST(drums, a_folder_becomes_a_kit)
{
    QTemporaryDir root;
    const QString samples = root.path() + "/Samples";
    const QString pack = samples + "/Acoustic";
    auto touch = [&](const QString& rel) {
        QDir().mkpath(QFileInfo(pack + "/" + rel).path());
        QFile f(pack + "/" + rel);
        f.open(QIODevice::WriteOnly);
    };
    for (const char* f : { "Kick/Kick Hard.wav", "Kick/Kick Soft.wav", "Kick/Kick Hard 2.wav",
                           "Snare Ghost.wav", "Snare.wav", "Hats/Closed Soft.wav", "Hats/Closed Tight.wav", "Hats/Open.wav",
                           "Toms/Tom 1.wav", "Toms/Tom 2.wav", "Toms/Tom 3.wav",
                           "Crash.wav", "Ride.wav", "Room Ambience.wav", "notes.txt" })
        touch(f);

    const auto fk = D::buildKitFromFolder(pack, samples);
    CHECK(fk.sampleCount == 14);   // the .txt isn't a sample
    CHECK(fk.kit.validate().isEmpty());
    CHECK(fk.unmatched == QStringList{ "Room Ambience.wav" });

    auto sampleOf = [&](int note) {
        const auto* p = fk.kit.padForNote(note);
        return p && p->source == D::PadSource::Sample ? p->sample : QString();
    };
    CHECK(sampleOf(36) == "Acoustic/Kick/Kick Hard.wav");   // not the soft layer; relative to the samples folder
    CHECK(fk.alternatives.value(36).size() == 2);
    CHECK(sampleOf(38) == "Acoustic/Snare.wav");
    CHECK(sampleOf(42) == "Acoustic/Hats/Closed Tight.wav");   // the soft take is shorter, but soft
    CHECK(sampleOf(46) == "Acoustic/Hats/Open.wav");
    // Three numbered toms: high, mid and floor.
    CHECK(sampleOf(50) == "Acoustic/Toms/Tom 1.wav");
    CHECK(sampleOf(47) == "Acoustic/Toms/Tom 2.wav");
    CHECK(sampleOf(41) == "Acoustic/Toms/Tom 3.wav");
    CHECK(sampleOf(49) == "Acoustic/Crash.wav");
    CHECK(sampleOf(51) == "Acoustic/Ride.wav");
    // Pads keep the default kit's names and mix (hats still choke each other).
    CHECK(fk.kit.padForNote(42)->name == "Closed Hat" && fk.kit.padForNote(42)->choke == 1);
    CHECK(fk.kit.padForNote(46)->choke == 1);
    // Unmatched notes keep their synth voice...
    CHECK(fk.kit.padForNote(39) && fk.kit.padForNote(39)->source == D::PadSource::Synth);
    CHECK(fk.kit.pads.size() == D::Kit::defaultKit().pads.size());
    // ...or are left out.
    const auto only = D::buildKitFromFolder(pack, samples, false);
    CHECK(only.kit.pads.size() == 9);
    CHECK(only.kit.validate().isEmpty());
    for (const auto& p : only.kit.pads) CHECK(p.source == D::PadSource::Sample);

    // Outside the samples folder, paths are absolute.
    const auto elsewhere = D::buildKitFromFolder(pack, root.path() + "/Other");
    CHECK(QDir::isAbsolutePath(elsewhere.chosen.value(38)));

    // Many numbered takes of one tom are one tom, not six.
    QTemporaryDir takes;
    for (int i = 1; i <= 8; ++i) {
        QFile f(takes.path() + QString("/Tom %1.wav").arg(i));
        f.open(QIODevice::WriteOnly);
    }
    const auto tk = D::buildKitFromFolder(takes.path(), samples, false);
    CHECK(tk.kit.pads.size() == 1 && tk.kit.pads[0].note == 47 && tk.alternatives.value(47).size() == 7);
}
