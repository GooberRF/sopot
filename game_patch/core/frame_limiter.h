#pragma once

#include "../misc/misc.h"
#include <windows.h>
#include <string>
#include <vector>

void frame_limiter_apply_settings(const Rf2PatchSettings& settings);
void frame_limiter_on_present();
void frame_limiter_on_device_reset();
void frame_limiter_draw_overlay(HWND target_window);
void frame_limiter_apply_runtime_overrides();
bool frame_limiter_is_active();
bool frame_limiter_is_vsync_enabled();

bool frame_limiter_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines);
