#include "camera.h"
#include "../rf2/player/camera.h"
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <windows.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace
{

constexpr float rf2_base_hfov_4_3 = 90.0f;
constexpr float rf2_base_aspect_4_3 = 4.0f / 3.0f;

float g_user_fov = 0.0f;
unsigned g_res_width = 1024;
unsigned g_res_height = 768;
std::string g_settings_path{};
std::vector<uintptr_t> g_fov_instruction_immediates{};
bool g_fov_sites_scanned = false;
bool g_warned_missing_fov_sites = false;
bool g_camera_fov_hook_installed = false;

int __cdecl set_camera_params_hook(int arg0, int arg4, int arg8, int arg_c, int arg_10, float arg_14);
FunHook<int __cdecl(int, int, int, int, int, float)> g_set_camera_params_hook{
    rf2::player::camera::set_camera_params_addr,
    set_camera_params_hook,
};

std::string trim_ascii_copy(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool starts_with_case_insensitive(std::string_view value, std::string_view prefix)
{
    if (prefix.size() > value.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        unsigned char lc = static_cast<unsigned char>(value[i]);
        unsigned char rc = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lc) != std::tolower(rc)) {
            return false;
        }
    }
    return true;
}

float clamp_fov(float value)
{
    return std::clamp(value, 1.0f, 179.0f);
}

float compute_auto_hfov(unsigned width, unsigned height)
{
    const float aspect = static_cast<float>(std::max<unsigned>(width, 1)) / static_cast<float>(std::max<unsigned>(height, 1));
    const float base_half_rad = (rf2_base_hfov_4_3 * 0.5f) * (3.1415926535f / 180.0f);
    const float scaled_half = std::atan(std::tan(base_half_rad) * (aspect / rf2_base_aspect_4_3));
    return clamp_fov(scaled_half * 2.0f * (180.0f / 3.1415926535f));
}

float get_target_hfov()
{
    if (g_user_fov <= 0.0f) {
        return compute_auto_hfov(g_res_width, g_res_height);
    }
    return clamp_fov(g_user_fov);
}

int __cdecl set_camera_params_hook(int arg0, int arg4, int arg8, int arg_c, int arg_10, float /*arg_14*/)
{
    return g_set_camera_params_hook.call_target(arg0, arg4, arg8, arg_c, arg_10, get_target_hfov());
}

void install_camera_fov_hook()
{
    if (g_camera_fov_hook_installed) {
        return;
    }

    const uint8_t first_op = addr_as_ref<uint8_t>(rf2::player::camera::set_camera_params_addr);
    if (first_op != 0xA0 && first_op != 0x55) {
        xlog::warn(
            "RF2 camera FOV hook signature mismatch at 0x{:X}",
            static_cast<unsigned>(rf2::player::camera::set_camera_params_addr));
        return;
    }

    g_set_camera_params_hook.install();
    g_camera_fov_hook_installed = true;
    xlog::info(
        "Installed RF2 camera FOV hook at 0x{:X}",
        static_cast<unsigned>(rf2::player::camera::set_camera_params_addr));
}

void save_fov_to_settings()
{
    if (g_settings_path.empty()) {
        return;
    }

    char fov_value[64] = {};
    if (g_user_fov <= 0.0f) {
        std::snprintf(fov_value, sizeof(fov_value), "0");
    }
    else {
        std::snprintf(fov_value, sizeof(fov_value), "%.3f", clamp_fov(g_user_fov));
    }
    if (!WritePrivateProfileStringA("sopot", "fov", fov_value, g_settings_path.c_str())) {
        xlog::warn("Failed to persist fov={} to {}", fov_value, g_settings_path);
    }
}

void discover_fov_instruction_sites()
{
    if (g_fov_sites_scanned) {
        return;
    }
    g_fov_sites_scanned = true;

    const uintptr_t base = rf2::module_base();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        xlog::warn("Unable to scan RF2 FOV sites: invalid DOS header");
        return;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        xlog::warn("Unable to scan RF2 FOV sites: invalid NT header");
        return;
    }

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_CNT_CODE) == 0) {
            continue;
        }
        const size_t section_size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (section_size < 10) {
            continue;
        }

        const auto* begin = reinterpret_cast<const uint8_t*>(base + section->VirtualAddress);
        for (size_t off = 0; off + 10 <= section_size; ++off) {
            if (begin[off] != 0xC7) {
                continue;
            }
            const uint8_t modrm = begin[off + 1];
            if (modrm < 0x80 || modrm > 0x87) {
                continue;
            }

            uint32_t disp = 0;
            std::memcpy(&disp, begin + off + 2, sizeof(disp));
            if (disp != rf2::player::camera::local_player_fov_offset) {
                continue;
            }

            const uintptr_t imm_addr = reinterpret_cast<uintptr_t>(begin + off + 6);
            if (std::find(g_fov_instruction_immediates.begin(), g_fov_instruction_immediates.end(), imm_addr)
                == g_fov_instruction_immediates.end()) {
                g_fov_instruction_immediates.push_back(imm_addr);
            }
        }
    }

    if (g_fov_instruction_immediates.size() > 32) {
        xlog::warn(
            "RF2 FOV scan found suspiciously high site count ({}), ignoring instruction patching",
            g_fov_instruction_immediates.size());
        g_fov_instruction_immediates.clear();
        return;
    }

    if (!g_fov_instruction_immediates.empty()) {
        xlog::info("Discovered {} RF2 FOV instruction site(s)", g_fov_instruction_immediates.size());
    }
}

size_t patch_fov_instruction_immediates(float fov)
{
    discover_fov_instruction_sites();
    for (uintptr_t imm_addr : g_fov_instruction_immediates) {
        write_mem<float>(imm_addr, fov);
    }
    if (g_fov_instruction_immediates.empty() && !g_warned_missing_fov_sites) {
        g_warned_missing_fov_sites = true;
        xlog::warn("No RF2 FOV instruction sites found; using player-instance fallback only");
    }
    return g_fov_instruction_immediates.size();
}

bool patch_local_player_fov(float fov)
{
    auto* player = rf2::player::camera::local_player_ptr();
    if (!player) {
        return false;
    }
    addr_as_ref<float>(reinterpret_cast<uintptr_t>(player) + rf2::player::camera::local_player_fov_offset) = fov;
    return true;
}

void apply_target_fov(bool log)
{
    const float target = get_target_hfov();
    const size_t patched_site_count = patch_fov_instruction_immediates(target);
    const bool local_player_patched = patch_local_player_fov(target);

    if (log) {
        xlog::info(
            "Applied RF2 FOV: setting={} target={} resolution={}x{} (sites={}, local_player={})",
            g_user_fov,
            target,
            g_res_width,
            g_res_height,
            patched_site_count,
            local_player_patched);
    }
}

bool parse_fov_command_value(std::string_view command, float& out_value)
{
    const std::string trimmed = trim_ascii_copy(std::string{command});
    if (!starts_with_case_insensitive(trimmed, "fov")) {
        return false;
    }
    if (trimmed.size() == 3) {
        out_value = g_user_fov;
        return true;
    }
    std::string remainder = trim_ascii_copy(trimmed.substr(3));
    if (remainder.empty()) {
        out_value = g_user_fov;
        return true;
    }
    try {
        out_value = std::stof(remainder);
        return true;
    }
    catch (...) {
        return false;
    }
}

} // namespace

void camera_apply_settings(const Rf2PatchSettings& settings)
{
    g_user_fov = std::max(settings.fov, 0.0f);
    g_res_width = std::max(settings.window_width, 320u);
    g_res_height = std::max(settings.window_height, 200u);
    g_settings_path = settings.settings_file_path;
    install_camera_fov_hook();
    apply_target_fov(true);
}

void camera_set_resolution(unsigned width, unsigned height)
{
    g_res_width = std::max(width, 1u);
    g_res_height = std::max(height, 1u);
    if (g_user_fov <= 0.0f) {
        apply_target_fov(false);
    }
}

bool camera_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines)
{
    out_success = false;
    out_status.clear();
    out_output_lines.clear();

    const std::string trimmed = trim_ascii_copy(command);
    if (!starts_with_case_insensitive(trimmed, "fov")) {
        return false;
    }

    float value = 0.0f;
    if (!parse_fov_command_value(trimmed, value)) {
        out_output_lines.push_back("Usage: fov <num>");
        out_output_lines.push_back("Use fov 0 to enable auto aspect-ratio scaling.");
        out_status = "Invalid fov value.";
        return true;
    }

    if (trimmed.size() == 3) {
        const float auto_fov = compute_auto_hfov(g_res_width, g_res_height);
        if (g_user_fov <= 0.0f) {
            char line[160] = {};
            std::snprintf(line, sizeof(line), "fov is auto (%.2f at %ux%u).", auto_fov, g_res_width, g_res_height);
            out_output_lines.emplace_back(line);
        }
        else {
            char line[200] = {};
            std::snprintf(
                line,
                sizeof(line),
                "fov is %.2f (manual). Auto would be %.2f at %ux%u.",
                clamp_fov(g_user_fov),
                auto_fov,
                g_res_width,
                g_res_height);
            out_output_lines.emplace_back(line);
        }
        out_status = "Printed fov state.";
        out_success = true;
        return true;
    }

    if (value < 0.0f) {
        out_output_lines.push_back("fov must be >= 0.");
        out_status = "Invalid fov value.";
        return true;
    }

    g_user_fov = value <= 0.0f ? 0.0f : clamp_fov(value);
    apply_target_fov(true);
    save_fov_to_settings();

    if (g_user_fov <= 0.0f) {
        char line[160] = {};
        std::snprintf(
            line,
            sizeof(line),
            "fov auto enabled: %.2f at %ux%u (baseline 90 at 4:3).",
            compute_auto_hfov(g_res_width, g_res_height),
            g_res_width,
            g_res_height);
        out_output_lines.emplace_back(line);
    }
    else {
        char line[96] = {};
        std::snprintf(line, sizeof(line), "fov set to %.2f.", g_user_fov);
        out_output_lines.emplace_back(line);
    }
    out_status = "Applied fov.";
    out_success = true;
    return true;
}
