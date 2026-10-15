#include "ModularEditorLink.h"

namespace OpenDaw {

ModularEditorLink& ModularEditorLink::instance()
{
    static ModularEditorLink link;
    return link;
}

} // namespace OpenDaw
