#pragma once

#include "Module.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace OpenDaw::Modular {

enum class PortKind { Audio, CV, Gate, Pitch };
enum class Section { Voice, Global, Both };

struct PortDef {
    std::string name;
    PortKind kind;
    std::string description;
};

struct ParamDef {
    std::string name;
    float min = 0.0f, max = 1.0f, defaultValue = 0.0f;
    std::string unit;                  // "Hz", "s", "semitones", ...
    std::vector<std::string> choices;  // non-empty: an enum stored as its index
    std::string description;
};

// Everything about one module type: the DSP factory, validation, the
// editor's panel and the catalog the AI reads all come from here.
struct ModuleDef {
    std::string type;
    std::string description;
    Section section;
    std::vector<PortDef> inputs;
    std::vector<PortDef> outputs;
    std::vector<ParamDef> params;
    std::function<std::unique_ptr<Module>()> create;

    int inputIndex(const std::string& name) const;
    int outputIndex(const std::string& name) const;
    int paramIndex(const std::string& name) const;
    bool allowedIn(Section s) const { return section == Section::Both || section == s; }
};

const std::vector<ModuleDef>& allModules();
const ModuleDef* findModule(const std::string& type);

const char* portKindName(PortKind k);
const char* sectionName(Section s);

} // namespace OpenDaw::Modular
