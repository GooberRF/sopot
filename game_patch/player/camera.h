#pragma once

#include "../misc/misc.h"
#include <string>
#include <vector>

void camera_apply_settings(const Rf2PatchSettings& settings);
void camera_set_resolution(unsigned width, unsigned height);

bool camera_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines);
