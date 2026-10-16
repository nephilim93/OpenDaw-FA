#pragma once

#include <QJsonArray>

namespace OpenDaw {

class AiToolDefs {
public:
    static QJsonArray allTools();
    static QJsonArray modularTools();  // AiModularTools.cpp
    static QJsonArray drumTools();     // AiDrumTools.cpp
};

} // namespace OpenDaw
