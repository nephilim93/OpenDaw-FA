#pragma once

#include "ModuleRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Small DSP and registry helpers shared by the module files.
namespace OpenDaw::Modular::detail {

inline constexpr double kTwoPi = 6.283185307179586;
inline constexpr double kC4Hz = 261.6255653005986;

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// Band-limited step correction for saw/square edges.
inline double polyBlep(double t, double dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// Fast deterministic white noise in -1..1.
struct NoiseGen {
    uint32_t state = 0x9E3779B9u;
    float next()
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<float>(state) * (2.0f / 4294967295.0f) - 1.0f;
    }
};

template <typename T> std::function<std::unique_ptr<Module>()> make()
{
    return [] { return std::make_unique<T>(); };
}

inline ParamDef param(std::string name, float min, float max, float def, std::string unit,
                      std::string description)
{
    return { std::move(name), min, max, def, std::move(unit), {}, std::move(description) };
}

inline ParamDef choice(std::string name, std::vector<std::string> choices, int def,
                       std::string description)
{
    const float max = static_cast<float>(choices.size() - 1);
    return { std::move(name), 0.0f, max, static_cast<float>(def), "", std::move(choices),
             std::move(description) };
}

} // namespace OpenDaw::Modular::detail
