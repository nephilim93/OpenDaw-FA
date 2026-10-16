#pragma once

#include <vector>

namespace OpenDaw::Modular {

// Ready-made Faust code modules, offered in the editor's Add Module menu.
// Each is a mono effect (one audio input, one output) the user can then edit.
struct CodeStarter {
    const char* name;          // also the new module's id base
    const char* description;
    const char* code;
};

const std::vector<CodeStarter>& codeStarters();

} // namespace OpenDaw::Modular
