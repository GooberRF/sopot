#pragma once
#include <string>
#include <vector>

enum class Rf2PatchWindowMode
{
    fullscreen,
    windowed,
    borderless,
};

struct Rf2PatchSettings
{
    Rf2PatchWindowMode window_mode = Rf2PatchWindowMode::windowed;
    unsigned window_width = 1024;
    unsigned window_height = 768;
    float fov = 0.0f;
    float max_fps = 0.0f;
    bool vsync = false;
    bool fast_start = true;
    bool direct_input_mouse = true;
    bool aim_slowdown_on_target = true;
    bool crosshair_enemy_indicator = true;
    bool r_showfps = false;
    std::string settings_file_path{};
};

void misc_apply_patches(const Rf2PatchSettings& settings);

bool misc_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines);
