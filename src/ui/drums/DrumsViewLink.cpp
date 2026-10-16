#include "DrumsViewLink.h"

namespace OpenDaw {

DrumsViewLink& DrumsViewLink::instance()
{
    static DrumsViewLink link;
    return link;
}

} // namespace OpenDaw
