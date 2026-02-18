#pragma once

#include <patch_common/MemUtils.h>

namespace rf2::player::autoaim
{
    // Auto-aim slowdown tuning globals used by RF2.
    static auto& slowdown_factor_min = addr_as_ref<float>(0x005EB770);
    static auto& slowdown_factor_max = addr_as_ref<float>(0x005EB76C);
    static auto& slowdown_rate = addr_as_ref<float>(0x005EB774);
}
