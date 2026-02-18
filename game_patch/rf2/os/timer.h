#pragma once

#include <patch_common/MemUtils.h>
#include <cstdint>

namespace rf2::os::timer
{
    constexpr uintptr_t timer_get_addr = 0x00540270;         // sub_540270
    constexpr uintptr_t frametime_reset_addr = 0x0054D040;   // sub_54D040
    constexpr uintptr_t set_timescale_addr = 0x0054D290;     // sub_54D290
    constexpr uintptr_t set_frametime_bounds_addr = 0x0054D2B0; // sub_54D2B0

    static auto& timer_get = addr_as_ref<int(int)>(timer_get_addr);
    static auto& frametime_reset = addr_as_ref<void()>(frametime_reset_addr);
    static auto& set_timescale = addr_as_ref<void(float)>(set_timescale_addr);
    static auto& set_frametime_bounds = addr_as_ref<int(float, float)>(set_frametime_bounds_addr);

    static auto& frametime_scaled = addr_as_ref<float>(0x005F56E4);
    static auto& frametime_raw = addr_as_ref<float>(0x005F56E8);
    static auto& timescale = addr_as_ref<float>(0x005F56F4);
    static auto& frametime_min = addr_as_ref<float>(0x005F56F8);
    static auto& frametime_max = addr_as_ref<float>(0x005F56FC);
}
