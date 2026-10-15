#pragma once

#include "ModuleRegistry.h"
#include <vector>

// Each module group registers its definitions from its own file.
namespace OpenDaw::Modular {

void addSequencingModules(std::vector<ModuleDef>& defs);  // ModulesSequencing.cpp
void addRandomModules(std::vector<ModuleDef>& defs);      // ModulesRandom.cpp
void addFxModules(std::vector<ModuleDef>& defs);          // ModulesFx.cpp

} // namespace OpenDaw::Modular
