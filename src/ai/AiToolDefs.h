#pragma once

#include <QJsonArray>

namespace OpenDaw {

class AiToolDefs {
public:
    static QJsonArray allTools();
    static QJsonArray modularTools();  // AiModularTools.cpp
};

} // namespace OpenDaw
