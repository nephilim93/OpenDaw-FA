#pragma once

#include <QObject>

namespace OpenDaw {

enum class EditTool {
    Pointer,
    Pen,
    Eraser,
    Split,
    Glue
};

// The single app-wide editing tool. The main toolbar and each editor's own
// tool buttons read and set it, so choosing a tool anywhere applies everywhere.
class EditToolState : public QObject {
    Q_OBJECT

public:
    static EditToolState& instance();

    EditTool tool() const { return tool_; }
    void setTool(EditTool tool);

signals:
    void toolChanged(EditTool tool);

private:
    EditToolState() = default;

    EditTool tool_ = EditTool::Pointer;
};

} // namespace OpenDaw
