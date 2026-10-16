#pragma once

#include "modular/ModuleRegistry.h"
#include <QString>

// How a modular parameter maps onto a 0..1 RotaryKnob, and how its value
// reads in labels and tooltips. Shared by the Modular editor's panels and
// the Drums tab's voice knobs, so the same parameter feels the same in both.
namespace OpenDaw::ParamKnob {

// Frequencies get a log knob so the musical range isn't squeezed into the
// first few degrees of travel.
bool isLog(const Modular::ParamDef& def);
double toKnob(const Modular::ParamDef& def, float value);
float fromKnob(const Modular::ParamDef& def, double n);

QString displayName(const std::string& name);
QString formatValue(const Modular::ParamDef& def, float v);
QString tooltip(const Modular::ParamDef& def, float v);

} // namespace OpenDaw::ParamKnob
