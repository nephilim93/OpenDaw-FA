#include "EditToolState.h"

namespace OpenDaw {

EditToolState& EditToolState::instance()
{
    static EditToolState state;
    return state;
}

void EditToolState::setTool(EditTool tool)
{
    if (tool == tool_) return;
    tool_ = tool;
    emit toolChanged(tool);
}

} // namespace OpenDaw
