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

// Code modules: a "faust" module's DSP is Faust source stored in the patch,
// so its ports and parameters come from compiling that source and each
// instance has its own definition. findModule("faust") returns a portless
// placeholder that renders silence (used when the source can't be compiled).
inline constexpr const char* kCodeModuleType = "faust";
inline constexpr int kMaxCodeModules = 8;               // per patch
inline constexpr int kMaxCodeSize = 64 * 1024;          // bytes of source
inline constexpr double kCodeCompileTimeout = 5.0;      // seconds
inline constexpr int kMaxCodePorts = 16;                // inputs, and outputs
inline constexpr int kMaxCodeParams = 64;

// True when OpenDaw was built with OPENDAW_WITH_FAUST.
bool codeModulesAvailable();

struct CodeCompileResult {
    std::shared_ptr<const ModuleDef> def;  // null on failure
    std::string error;                     // "line 3: syntax error", ...
};

// Compiles Faust source into a module definition. Message thread only: it
// can take a while, and the definition's create() makes DSP instances.
// `name` labels the source in compiler messages (the module id).
CodeCompileResult compileCodeModule(const std::string& name, const std::string& source,
                                    double timeoutSeconds = kCodeCompileTimeout);

const char* portKindName(PortKind k);
const char* sectionName(Section s);

} // namespace OpenDaw::Modular
