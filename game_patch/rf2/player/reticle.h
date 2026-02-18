#pragma once

#include <array>
#include <cstdint>

namespace rf2::player::reticle
{
    struct EnemyVariantPair
    {
        uintptr_t normal_ptr_addr;
        uintptr_t enemy_ptr_addr;
    };

    // Pointers to reticle texture name globals. Enemy pointer is swapped when aiming at enemies.
    constexpr std::array<EnemyVariantPair, 20> enemy_variant_pairs{{
        {0x005EDADC, 0x005EDAE0}, // sub
        {0x005EE328, 0x005EE32C}, // tank
        {0x005EE4D8, 0x005EE4DC}, // turret
        {0x005EE61C, 0x005EE620}, // sniper
        {0x005EE888, 0x005EE88C}, // smg
        {0x005EEA60, 0x005EEA64}, // shotgun
        {0x005EEC6C, 0x005EEC70}, // rail
        {0x005EEF68, 0x005EEF6C}, // quill
        {0x005EF270, 0x005EF274}, // nano_mp
        {0x005EF4C4, 0x005EF4C8}, // nano_mp variant
        {0x005EF61C, 0x005EF620}, // mp
        {0x005EF898, 0x005EF89C}, // mp variant
        {0x005EF95C, 0x005EF960}, // rail variant
        {0x005EFAEC, 0x005EFAF0}, // hmg
        {0x005EFDA8, 0x005EFDAC}, // gl
        {0x005F0070, 0x005F0074}, // glock
        {0x005F02D8, 0x005F02DC}, // glock variant
        {0x005F0440, 0x005F0444}, // assault
        {0x005F06B8, 0x005F06BC}, // anti
        {0x005F0920, 0x005F0924}, // nicw
    }};
}
