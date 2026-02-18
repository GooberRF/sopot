#pragma once

#include "../rf2.h"
#include <patch_common/MemUtils.h>
#include <cstdint>

namespace rf2::player::camera
{
    constexpr uintptr_t local_player_ptr_rva = 0x00211C98;
    constexpr uintptr_t local_player_fov_offset = 0x64C;

    constexpr uintptr_t set_camera_params_addr = 0x004C2080;
    static auto& set_camera_params = addr_as_ref<int(int, int, int, int, int, float)>(set_camera_params_addr);

    inline uint8_t* local_player_ptr()
    {
        return addr_as_ref<uint8_t*>(rf2::module_base() + local_player_ptr_rva);
    }
}
