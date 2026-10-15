#pragma once

#include "ModuleRegistry.h"
#include <vector>

// Each module group registers its definitions from its own file.
namespace OpenDaw::Modular {

void addSequencingModules(std::vector<ModuleDef>& defs);  // ModulesSequencing.cpp
void addRandomModules(std::vector<ModuleDef>& defs);      // ModulesRandom.cpp
void addFxModules(std::vector<ModuleDef>& defs);          // ModulesFx.cpp
void addExtraModules(std::vector<ModuleDef>& defs);       // ModulesExtra.cpp
void addCodeModules(std::vector<ModuleDef>& defs);        // FaustModule.cpp

} // namespace OpenDaw::Modular
