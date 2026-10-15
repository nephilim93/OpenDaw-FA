#include "TestHarness.h"
#include <set>

using namespace OpenDaw::Tests;

TEST(catalog, lists_every_module)
{
    const auto& defs = OpenDaw::Modular::allModules();
    const QJsonArray catalog = OpenDaw::Modular::catalogJson();
    CHECK(catalog.size() == qsizetype(defs.size()));
    CHECK(defs.size() >= 20);

    std::set<std::string> types;
    for (qsizetype i = 0; i < catalog.size() && i < qsizetype(defs.size()); ++i) {
        const auto& def = defs[size_t(i)];
        const auto entry = catalog[i].toObject();
        const std::string type = def.type;
        CHECK_MSG(types.insert(type).second, "duplicate type " + type);
        CHECK_MSG(entry["type"].toString().toStdString() == type, type);
        CHECK_MSG(!entry["description"].toString().isEmpty(), type);
        CHECK_MSG(entry["section"].toString() == OpenDaw::Modular::sectionName(def.section), type);
        CHECK_MSG(OpenDaw::Modular::findModule(type) == &def, type);
        CHECK_MSG(def.create && def.create() != nullptr, type);

        auto checkPorts = [&](const char* key, const std::vector<OpenDaw::Modular::PortDef>& ports) {
            const auto arr = entry[key].toArray();
            CHECK_MSG(arr.size() == qsizetype(ports.size()), type + " " + key);
            for (qsizetype p = 0; p < arr.size() && p < qsizetype(ports.size()); ++p) {
                const auto o = arr[p].toObject();
                CHECK_MSG(o["name"].toString().toStdString() == ports[size_t(p)].name, type);
                CHECK_MSG(o["kind"].toString() == OpenDaw::Modular::portKindName(ports[size_t(p)].kind), type);
            }
        };
        checkPorts("inputs", def.inputs);
        checkPorts("outputs", def.outputs);
        CHECK_MSG(int(def.inputs.size()) <= OpenDaw::Modular::CompiledPatch::kMaxInputs, type);

        const auto params = entry["params"].toArray();
        CHECK_MSG(params.size() == qsizetype(def.params.size()), type);
        for (qsizetype p = 0; p < params.size() && p < qsizetype(def.params.size()); ++p) {
            const auto o = params[p].toObject();
            const auto& pd = def.params[size_t(p)];
            const std::string where = type + "." + pd.name;
            CHECK_MSG(o["name"].toString().toStdString() == pd.name, where);
            CHECK_MSG(o.contains("default"), where);
            CHECK_MSG(pd.min <= pd.defaultValue && pd.defaultValue <= pd.max, where);
            if (pd.choices.empty()) {
                CHECK_MSG(o["min"].toDouble() == double(pd.min) && o["max"].toDouble() == double(pd.max), where);
                CHECK_MSG(o["default"].isDouble(), where);
            } else {
                CHECK_MSG(o["choices"].toArray().size() == qsizetype(pd.choices.size()), where);
                CHECK_MSG(o["default"].toString().toStdString()
                              == pd.choices[size_t(pd.defaultValue)], where);
            }
        }
    }

    // The groups registered from their own files are all there.
    for (const char* type : { "voice_in", "vco", "adsr", "clock", "step_sequencer", "transport",
                              "quantizer", "random", "sample_hold", "delay", "reverb" })
        CHECK_MSG(types.count(type) == 1, type);
}
