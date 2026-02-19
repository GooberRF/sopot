#pragma once

#include <patch_common/MemUtils.h>
#include <cstdint>

namespace rf2::os::input
{
    constexpr uintptr_t reset_key_state_addr = 0x0053D1A0;  // sub_53D1A0
    constexpr uintptr_t reset_edge_state_addr = 0x0053DB20; // sub_53DB20
    constexpr uintptr_t mouse_update_addr = 0x00539330;     // sub_539330
    constexpr uintptr_t mouse_set_sensitivity_addr = 0x00539AE0; // sub_539AE0
    constexpr uintptr_t mouse_get_delta_addr = 0x00539E60; // sub_539E60
    constexpr uintptr_t mouse_init_direct_input_addr = 0x00539890; // sub_539890
    constexpr uintptr_t mouse_release_direct_input_addr = 0x00539A20; // sub_539A20

    static auto& reset_key_state = addr_as_ref<void()>(reset_key_state_addr);
    static auto& reset_edge_state = addr_as_ref<void()>(reset_edge_state_addr);
    static auto& mouse_update = addr_as_ref<void()>(mouse_update_addr);
    static auto& mouse_set_sensitivity = addr_as_ref<int(float, float)>(mouse_set_sensitivity_addr);
    static auto& mouse_get_delta = addr_as_ref<void(int*, int*, int*)>(mouse_get_delta_addr);
    static auto& mouse_init_direct_input = addr_as_ref<int()>(mouse_init_direct_input_addr);
    static auto& mouse_release_direct_input = addr_as_ref<void()>(mouse_release_direct_input_addr);

    static auto& alt_key_down = addr_as_ref<uint8_t>(0x00B633BC);
    static auto& tab_key_down = addr_as_ref<uint8_t>(0x00B633BD);

    static auto& direct_input_api = addr_as_ref<void*>(0x00C602BC);
    static auto& mouse_aim_sensitivity_y = addr_as_ref<float>(0x006F2034);
    static auto& mouse_aim_sensitivity_x = addr_as_ref<float>(0x006F2038);
    static auto& mouse_range_x = addr_as_ref<int>(0x00BAF7AC);
    static auto& mouse_range_y = addr_as_ref<int>(0x00BAF7B4);
    static auto& mouse_sensitivity_x = addr_as_ref<float>(0x00BAF928);
    static auto& mouse_sensitivity_y = addr_as_ref<float>(0x00BAF92C);
    static auto& mouse_device = addr_as_ref<void*>(0x00BAF930);
    static auto& mouse_init_state = addr_as_ref<int>(0x00BAF934);
    static auto& mouse_direct_input_enabled = addr_as_ref<uint8_t>(0x00BAF938);
    static auto& mouse_system_initialized = addr_as_ref<uint8_t>(0x00BAF93A);

    constexpr uintptr_t control_profiles_addr = 0x01057FB8;
    constexpr uintptr_t control_profile_count_addr = 0x01058FF8;
    constexpr uintptr_t active_control_profile_addr = 0x00BAF77C;
    constexpr uintptr_t active_mouse_control_profile_addr = 0x00BAF798;
    constexpr uintptr_t control_profile_size = 0x104;
    constexpr uintptr_t control_profile_look_horizontal_sensitivity_offset = 0xE0;
    constexpr uintptr_t control_profile_look_vertical_sensitivity_offset = 0xE4;
    constexpr int max_control_profiles = 16;

    static auto& control_profile_count = addr_as_ref<int>(control_profile_count_addr);
    static auto& active_control_profile = addr_as_ref<int>(active_control_profile_addr);
    static auto& active_mouse_control_profile = addr_as_ref<int>(active_mouse_control_profile_addr);
}
