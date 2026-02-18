#pragma once

#include <patch_common/MemUtils.h>
#include <cstdint>

namespace rf2::os::input
{
    constexpr uintptr_t reset_key_state_addr = 0x0053D1A0;  // sub_53D1A0
    constexpr uintptr_t reset_edge_state_addr = 0x0053DB20; // sub_53DB20
    constexpr uintptr_t mouse_update_addr = 0x00539330;     // sub_539330
    constexpr uintptr_t mouse_init_direct_input_addr = 0x00539890; // sub_539890
    constexpr uintptr_t mouse_release_direct_input_addr = 0x00539A20; // sub_539A20

    static auto& reset_key_state = addr_as_ref<void()>(reset_key_state_addr);
    static auto& reset_edge_state = addr_as_ref<void()>(reset_edge_state_addr);
    static auto& mouse_update = addr_as_ref<void()>(mouse_update_addr);
    static auto& mouse_init_direct_input = addr_as_ref<int()>(mouse_init_direct_input_addr);
    static auto& mouse_release_direct_input = addr_as_ref<void()>(mouse_release_direct_input_addr);

    static auto& alt_key_down = addr_as_ref<uint8_t>(0x00B633BC);
    static auto& tab_key_down = addr_as_ref<uint8_t>(0x00B633BD);

    static auto& direct_input_api = addr_as_ref<void*>(0x00C602BC);
    static auto& mouse_device = addr_as_ref<void*>(0x00BAF930);
    static auto& mouse_init_state = addr_as_ref<int>(0x00BAF934);
    static auto& mouse_direct_input_enabled = addr_as_ref<uint8_t>(0x00BAF938);
    static auto& mouse_system_initialized = addr_as_ref<uint8_t>(0x00BAF93A);
}
