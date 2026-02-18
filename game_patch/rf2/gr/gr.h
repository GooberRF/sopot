#pragma once

#include <patch_common/MemUtils.h>
#include <windows.h>
#include <cstdint>

namespace rf2::gr
{
    enum class WindowMode : int
    {
        windowed = 0xC8,
        fullscreen = 0xC9,
    };

    constexpr uintptr_t set_video_mode_addr = 0x00535420;
    constexpr uintptr_t get_width_addr = 0x00535910;
    constexpr uintptr_t get_height_addr = 0x00535920;
    constexpr uintptr_t get_viewport_width_addr = 0x00535B70;
    constexpr uintptr_t get_viewport_height_addr = 0x00535B80;
    constexpr uintptr_t is_window_active_addr = 0x005304F0;
    constexpr uintptr_t init_mouse_surface_addr = 0x005397D0;
    constexpr uintptr_t play_movie_addr = 0x00537610;

    static auto& set_video_mode = addr_as_ref<int(int, int, int, int, int, HWND, int, int, int, int)>(set_video_mode_addr);
    static auto& get_width = addr_as_ref<int()>(get_width_addr);
    static auto& get_height = addr_as_ref<int()>(get_height_addr);
    static auto& get_viewport_width = addr_as_ref<int()>(get_viewport_width_addr);
    static auto& get_viewport_height = addr_as_ref<int()>(get_viewport_height_addr);
    static auto& is_window_active = addr_as_ref<uint8_t()>(is_window_active_addr);
    static auto& init_mouse_surface = addr_as_ref<void(int, int)>(init_mouse_surface_addr);
    static auto& play_movie = addr_as_ref<int(const char*, int)>(play_movie_addr);

    static auto& window_width = addr_as_ref<int>(0x00BAC174);
    static auto& window_height = addr_as_ref<int>(0x00BAC178);
    static auto& clip_width = addr_as_ref<int>(0x00BAC1A4);
    static auto& clip_height = addr_as_ref<int>(0x00BAC1A8);
    static auto& clip_right = addr_as_ref<int>(0x00BAC1B8);
    static auto& clip_bottom = addr_as_ref<int>(0x00BAC1C0);
    static auto& mouse_limit_x = addr_as_ref<int>(0x00B88A74);
    static auto& mouse_limit_y = addr_as_ref<int>(0x00B88A78);
    static auto& mouse_surface_width = addr_as_ref<int>(0x00B60DC4);
    static auto& mouse_surface_height = addr_as_ref<int>(0x00B60DC8);
    static auto& aspect_4_3_scale = addr_as_ref<float>(0x00BAC188);
    static auto& backbuffer_width = addr_as_ref<int>(0x00BAF7AC);
    static auto& backbuffer_height = addr_as_ref<int>(0x00BAF7B4);
    static auto& window_mode = addr_as_ref<int>(0x00BAC180);

    static auto& cap_framerate_to_vsync = addr_as_ref<uint8_t>(0x005F0EDD);
    static auto& always_block_for_vsync = addr_as_ref<uint8_t>(0x00B61F38);
}
