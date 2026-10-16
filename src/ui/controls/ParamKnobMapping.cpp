#include "ParamKnobMapping.h"
#include <algorithm>
#include <cmath>

namespace OpenDaw::ParamKnob {

bool isLog(const Modular::ParamDef& def)
{
    return def.unit == "Hz" && def.min > 0.0f && def.max > def.min;
}

double toKnob(const Modular::ParamDef& def, float value)
{
    if (def.max <= def.min) return 0.0;
    if (isLog(def))
        return std::log(std::max(value, def.min) / def.min) / std::log(def.max / def.min);
    return (value - def.min) / (def.max - def.min);
}

float fromKnob(const Modular::ParamDef& def, double n)
{
    n = std::clamp(n, 0.0, 1.0);
    if (isLog(def))
        return float(def.min * std::pow(double(def.max / def.min), n));
    return float(def.min + n * (def.max - def.min));
}

QString displayName(const std::string& name)
{
    return QString::fromStdString(name).replace('_', ' ');
}

QString formatValue(const Modular::ParamDef& def, float v)
{
    const float a = std::abs(v);
    const int decimals = a >= 100.0f ? 0 : a >= 10.0f ? 1 : 2;
    QString text = QString::number(double(v), 'f', decimals);
    if (!def.unit.empty()) text += " " + QString::fromStdString(def.unit);
    return text;
}

QString tooltip(const Modular::ParamDef& def, float v)
{
    QString tip = displayName(def.name) + ": " + formatValue(def, v);
    if (!def.description.empty()) tip += "\n" + QString::fromStdString(def.description);
    return tip;
}

} // namespace OpenDaw::ParamKnob
