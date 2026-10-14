#include "JuceQtBridge.h"
#include <juce_events/juce_events.h>

namespace OpenDaw {

JuceQtBridge::JuceQtBridge(QObject* parent)
    : QObject(parent)
{
    // Dispatch whatever is pending and return immediately. A non-zero
    // duration blocks the Qt thread for that long on every tick.
    connect(&pumpTimer_, &QTimer::timeout, this, []() {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            mm->runDispatchLoopUntil(0);
    });
}

JuceQtBridge::~JuceQtBridge()
{
    stop();
}

void JuceQtBridge::start()
{
    // On macOS and Windows JUCE posts its messages to the native event queue
    // (the main CFRunLoop / a hidden message window), which Qt's event loop
    // already dispatches. Only Linux has a separate JUCE loop to pump.
#if JUCE_LINUX || JUCE_BSD
    pumpTimer_.start(10);
#endif
}

void JuceQtBridge::stop()
{
    pumpTimer_.stop();
}

} // namespace OpenDaw
