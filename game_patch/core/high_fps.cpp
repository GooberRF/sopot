#include "high_fps.h"
#include <xlog/xlog.h>

namespace
{
bool g_patch_attempted = false;
}

void high_fps_apply_patch()
{
    if (g_patch_attempted) {
        return;
    }
    g_patch_attempted = true;

    // Disabled for now: global __ftol interception is too risky in RF2 and can
    // corrupt gameplay object state. We'll replace this with targeted call-site
    // fixes like Alpine once each problematic site is validated.
    xlog::warn("RF2 high-fps __ftol patch is temporarily disabled for stability.");
}
