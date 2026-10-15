#include "VelocityFxPlugin.h"

namespace OpenDaw {

namespace {
const juce::Identifier kScaleId("velocityScale");
const juce::Identifier kOffsetId("velocityOffset");
const juce::Identifier kRandomId("velocityRandom");
const juce::Identifier kMinId("velocityMin");
const juce::Identifier kMaxId("velocityMax");

juce::String asInt(float v) { return juce::String(juce::roundToInt(v)); }
}

const char* VelocityFxPlugin::xmlTypeName = "openDawVelocityFx";

VelocityFxPlugin::VelocityFxPlugin(te::PluginCreationInfo info)
    : te::Plugin(info)
{
    auto* um = getUndoManager();

    scale = addParam("scale", "Scale", { 0.0f, 200.0f },
                     [](float v) { return asInt(v) + "%"; },
                     [](const juce::String& s) { return s.getFloatValue(); });
    offset = addParam("offset", "Offset", { -127.0f, 127.0f },
                      [](float v) { return (v > 0.5f ? "+" : "") + asInt(v); },
                      [](const juce::String& s) { return s.getFloatValue(); });
    random = addParam("random", "Random", { 0.0f, 64.0f },
                      [](float v) { return "+/-" + asInt(v); },
                      [](const juce::String& s) { return s.getFloatValue(); });
    minimum = addParam("min", "Min", { 1.0f, 127.0f }, asInt,
                       [](const juce::String& s) { return s.getFloatValue(); });
    maximum = addParam("max", "Max", { 1.0f, 127.0f }, asInt,
                       [](const juce::String& s) { return s.getFloatValue(); });

    scaleValue.referTo(state, kScaleId, um, 100.0f);
    offsetValue.referTo(state, kOffsetId, um, 0.0f);
    randomValue.referTo(state, kRandomId, um, 0.0f);
    minValue.referTo(state, kMinId, um, 1.0f);
    maxValue.referTo(state, kMaxId, um, 127.0f);

    scale->attachToCurrentValue(scaleValue);
    offset->attachToCurrentValue(offsetValue);
    random->attachToCurrentValue(randomValue);
    minimum->attachToCurrentValue(minValue);
    maximum->attachToCurrentValue(maxValue);
}

VelocityFxPlugin::~VelocityFxPlugin()
{
    notifyListenersOfDeletion();

    for (auto* p : { scale.get(), offset.get(), random.get(), minimum.get(), maximum.get() })
        p->detachFromCurrentValue();
}

int VelocityFxPlugin::transform(int velocity)
{
    double v = velocity * scale->getCurrentValue() / 100.0 + offset->getCurrentValue();

    const float spread = random->getCurrentValue();
    if (spread >= 0.5f)
        v += (rng.nextFloat() * 2.0f - 1.0f) * spread;

    float lo = minimum->getCurrentValue();
    float hi = maximum->getCurrentValue();
    if (lo > hi) std::swap(lo, hi);

    return juce::jlimit(juce::roundToInt(lo), juce::roundToInt(hi), juce::roundToInt(v));
}

void VelocityFxPlugin::applyToBuffer(const te::PluginRenderContext& fc)
{
    if (fc.bufferForMidiMessages == nullptr) return;

    // A note-on with velocity 0 is a note-off, so leave those alone.
    for (auto& m : *fc.bufferForMidiMessages)
        if (m.isNoteOn())
            m.setVelocity(transform(m.getVelocity()) / 127.0f);
}

void VelocityFxPlugin::restorePluginStateFromValueTree(const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues(v, scaleValue, offsetValue, randomValue, minValue, maxValue);

    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
}

} // namespace OpenDaw
