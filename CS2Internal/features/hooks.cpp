// AUDIT: P1-7, P1-9, P1-12, P1-13, P2-8, P3-7
#include "hooks.h"
#include "../features/misc.h"

namespace Hooks {

void RunWorldHooks()
{
    // P1-13: Misc::Apply() is the single owner of per-frame misc features.
    // It is not called from the render frame, so this is the only call site.
    // P1-9/P3-7: thirdperson is handled by Misc::RunThirdPerson() using the
    // dynamically resolved CCSGOInput base; no hardcoded key or offset here.
    Misc::Apply();
}

} // namespace Hooks
