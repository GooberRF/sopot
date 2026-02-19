#include "frame_limiter.h"
#include "../rf2/gr/gr.h"
#include "../rf2/os/timer.h"
#include <patch_common/FunHook.h>
#include <windows.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace
{

constexpr float min_configurable_max_fps = 10.0f;
constexpr float max_configurable_max_fps = 240.0f;
constexpr float default_max_fps = 240.0f;
constexpr float default_frametime_min = 0.0f;
constexpr float default_frametime_max = 0.25f;
constexpr COLORREF fps_overlay_color = RGB(0, 255, 0);
constexpr int fps_overlay_margin_px = 8;
constexpr int fps_overlay_width_px = 220;
constexpr int fps_overlay_height_px = 48;

float g_max_fps = default_max_fps;
bool g_vsync_enabled = false;
bool g_show_fps_overlay = false;
bool g_experimental_fps_stabilization_enabled = false;
std::string g_settings_path{};
bool g_logged_vsync_disable = false;
bool g_hooks_installed = false;
int g_frametime_reset_log_count = 0;
LARGE_INTEGER g_qpc_frequency{};
bool g_qpc_initialized = false;
bool g_present_limiter_initialized = false;
long long g_next_present_tick = 0;
long long g_last_present_tick = 0;
long long g_present_fps_sample_tick = 0;
unsigned g_present_fps_sample_count = 0;
float g_draw_fps = 0.0f;
float g_sim_fps = 0.0f;
HFONT g_overlay_font = nullptr;

void __cdecl frametime_reset_hook();
FunHook<void __cdecl()> g_frametime_reset_hook{
    rf2::os::timer::frametime_reset_addr,
    frametime_reset_hook,
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

bool parse_bool_like(std::string_view value, bool& out_value)
{
    std::string trimmed = trim_ascii_copy(std::string{value});
    if (trimmed.empty()) {
        return false;
    }

    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (trimmed == "1" || trimmed == "true" || trimmed == "on" || trimmed == "yes")
    {
        out_value = true;
        return true;
    }

    if (trimmed == "0" || trimmed == "false" || trimmed == "off" || trimmed == "no")
    {
        out_value = false;
        return true;
    }

    return false;
}

float clamp_max_fps(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(value, min_configurable_max_fps, max_configurable_max_fps);
}

float get_effective_max_fps()
{
    return clamp_max_fps(g_max_fps);
}

void apply_frametime_limits(bool log)
{
    const float frametime_min = default_frametime_min;

    rf2::os::timer::set_frametime_bounds(frametime_min, default_frametime_max);
    rf2::os::timer::frametime_min = frametime_min;
    rf2::os::timer::frametime_max = default_frametime_max;

    if (log) {
        xlog::info(
            "Applied RF2 frametime bounds: frametime_min={}, frametime_max={} (render cap handled in Present hook)",
            frametime_min,
            default_frametime_max);
    }
}

void reset_present_limiter_state()
{
    g_present_limiter_initialized = false;
    g_next_present_tick = 0;
    g_last_present_tick = 0;
    g_present_fps_sample_tick = 0;
    g_present_fps_sample_count = 0;
    g_draw_fps = 0.0f;
    g_sim_fps = 0.0f;
}

void ensure_qpc_initialized()
{
    if (g_qpc_initialized) {
        return;
    }

    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        return;
    }

    g_qpc_frequency = freq;
    g_qpc_initialized = true;
}

void install_hooks_if_needed()
{
    if (g_hooks_installed) {
        return;
    }

    g_frametime_reset_hook.install();
    g_hooks_installed = true;
    xlog::info("Installed RF2 frametime reset hook");
}

void __cdecl frametime_reset_hook()
{
    g_frametime_reset_hook.call_target();
    apply_frametime_limits(false);

    if (g_frametime_reset_log_count < 4) {
        ++g_frametime_reset_log_count;
        xlog::info("Reapplied RF2 frametime limits after frametime reset");
    }
}

void enforce_present_fps_cap()
{
    const float max_fps = get_effective_max_fps();
    if (max_fps <= 0.0f || g_vsync_enabled) {
        return;
    }

    ensure_qpc_initialized();
    if (!g_qpc_initialized || g_qpc_frequency.QuadPart <= 0) {
        return;
    }

    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }

    const long long target_frame_ticks = std::max<long long>(
        1,
        static_cast<long long>(std::llround(static_cast<double>(g_qpc_frequency.QuadPart) / max_fps)));
    if (!g_present_limiter_initialized) {
        g_present_limiter_initialized = true;
        g_next_present_tick = now.QuadPart + target_frame_ticks;
        return;
    }

    if (now.QuadPart < g_next_present_tick) {
        const long long wait_ticks = g_next_present_tick - now.QuadPart;
        const long long wait_ms = (wait_ticks * 1000) / g_qpc_frequency.QuadPart;
        if (wait_ms > 1) {
            Sleep(static_cast<DWORD>(wait_ms - 1));
        }

        do {
            Sleep(0);
            if (!QueryPerformanceCounter(&now)) {
                break;
            }
        } while (now.QuadPart < g_next_present_tick);
    }

    if (now.QuadPart > g_next_present_tick + (target_frame_ticks * 4)) {
        // Big hitch; reset phase so limiter does not oscillate after a stall.
        g_next_present_tick = now.QuadPart + target_frame_ticks;
    }
    else {
        g_next_present_tick += target_frame_ticks;
        if (g_next_present_tick < now.QuadPart) {
            g_next_present_tick = now.QuadPart + target_frame_ticks;
        }
    }
}

void update_fps_metrics()
{
    ensure_qpc_initialized();
    if (!g_qpc_initialized || g_qpc_frequency.QuadPart <= 0) {
        return;
    }

    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }

    if (g_last_present_tick != 0 && now.QuadPart > g_last_present_tick) {
        const double delta_sec =
            static_cast<double>(now.QuadPart - g_last_present_tick) / static_cast<double>(g_qpc_frequency.QuadPart);
        if (delta_sec > 0.000001) {
            const float inst_draw_fps = static_cast<float>(1.0 / delta_sec);
            if (g_draw_fps <= 0.0f) {
                g_draw_fps = inst_draw_fps;
            }
            else {
                g_draw_fps = (g_draw_fps * 0.90f) + (inst_draw_fps * 0.10f);
            }
        }
    }
    g_last_present_tick = now.QuadPart;

    ++g_present_fps_sample_count;
    if (g_present_fps_sample_tick == 0) {
        g_present_fps_sample_tick = now.QuadPart;
    }
    else {
        const long long elapsed = now.QuadPart - g_present_fps_sample_tick;
        if (elapsed >= (g_qpc_frequency.QuadPart / 4)) {
            const float elapsed_sec = static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(g_qpc_frequency.QuadPart));
            if (elapsed_sec > 0.0f) {
                const float sampled_draw_fps = static_cast<float>(g_present_fps_sample_count) / elapsed_sec;
                g_draw_fps = (g_draw_fps <= 0.0f)
                    ? sampled_draw_fps
                    : ((g_draw_fps * 0.75f) + (sampled_draw_fps * 0.25f));
            }
            g_present_fps_sample_count = 0;
            g_present_fps_sample_tick = now.QuadPart;
        }
    }

    const float sim_dt = rf2::os::timer::frametime_scaled;
    if (sim_dt > 0.000001f) {
        const float inst_sim_fps = 1.0f / sim_dt;
        if (g_sim_fps <= 0.0f) {
            g_sim_fps = inst_sim_fps;
        }
        else {
            g_sim_fps = (g_sim_fps * 0.90f) + (inst_sim_fps * 0.10f);
        }
    }
}

void ensure_overlay_font()
{
    if (g_overlay_font) {
        return;
    }

    g_overlay_font = CreateFontA(
        -18,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        "Consolas");
    if (!g_overlay_font) {
        g_overlay_font = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
    }
}

void save_showfps_to_settings()
{
    if (g_settings_path.empty()) {
        return;
    }

    if (!WritePrivateProfileStringA(
            "sopot",
            "r_showfps",
            g_show_fps_overlay ? "1" : "0",
            g_settings_path.c_str()))
    {
        xlog::warn(
            "Failed to persist r_showfps={} to {}",
            g_show_fps_overlay ? 1 : 0,
            g_settings_path);
    }
}

void save_max_fps_to_settings()
{
    if (g_settings_path.empty()) {
        return;
    }

    char value[64] = {};
    if (g_max_fps <= 0.0f) {
        std::snprintf(value, sizeof(value), "0");
    }
    else {
        std::snprintf(value, sizeof(value), "%.3f", g_max_fps);
    }
    if (!WritePrivateProfileStringA("sopot", "max_fps", value, g_settings_path.c_str())) {
        xlog::warn("Failed to persist max_fps={} to {}", value, g_settings_path);
    }
}

bool parse_max_fps_command_value(std::string_view command, float& out_value)
{
    const std::string trimmed = trim_ascii_copy(std::string{command});
    if (!starts_with_case_insensitive(trimmed, "maxfps")) {
        return false;
    }

    if (trimmed.size() == 6) {
        out_value = g_max_fps;
        return true;
    }

    std::string remainder = trim_ascii_copy(trimmed.substr(6));
    if (remainder.empty()) {
        out_value = g_max_fps;
        return true;
    }

    try {
        out_value = std::stof(remainder);
        return std::isfinite(out_value);
    }
    catch (...) {
        return false;
    }
}

} // namespace

void frame_limiter_apply_runtime_overrides()
{
    rf2::gr::cap_framerate_to_vsync = g_vsync_enabled ? 1 : 0;
    rf2::gr::always_block_for_vsync = g_vsync_enabled ? 1 : 0;
    if (!g_logged_vsync_disable) {
        g_logged_vsync_disable = true;
        xlog::info(
            "Applied RF2 vsync state (cap_framerate_to_vsync={}, always_block_for_vsync={})",
            g_vsync_enabled ? 1 : 0,
            g_vsync_enabled ? 1 : 0);
    }
}

void frame_limiter_apply_settings(const Rf2PatchSettings& settings)
{
    g_settings_path = settings.settings_file_path;
    const float requested_max_fps = std::max(settings.max_fps, 0.0f);
    g_max_fps = clamp_max_fps(requested_max_fps);
    g_vsync_enabled = settings.vsync;
    g_show_fps_overlay = settings.r_showfps;
    g_experimental_fps_stabilization_enabled = settings.experimental_fps_stabilization;
    g_logged_vsync_disable = false;

    frame_limiter_apply_runtime_overrides();
    reset_present_limiter_state();

    if (!g_experimental_fps_stabilization_enabled) {
        xlog::info(
            "Experimental FPS stabilization is disabled (experimental_fps_stabilization=0).");
        if (requested_max_fps > 0.0f) {
            xlog::warn(
                "max_fps={} configured but experimental FPS stabilization is disabled; render cap will not be enforced.",
                requested_max_fps);
        }
        if (g_show_fps_overlay) {
            xlog::warn(
                "r_showfps=1 configured but experimental FPS stabilization is disabled; FPS overlay will not be shown.");
        }
        return;
    }

    install_hooks_if_needed();
    apply_frametime_limits(true);

    xlog::info(
        "Applied frame limiter settings (experimental): requested_max_fps={}, effective_max_fps={} (0=uncapped), vsync={}, r_showfps={} (render cap in Present)",
        requested_max_fps,
        get_effective_max_fps(),
        g_vsync_enabled ? 1 : 0,
        g_show_fps_overlay ? 1 : 0);
    if (requested_max_fps > max_configurable_max_fps) {
        xlog::warn(
            "Configured max_fps={} exceeds safety clamp; clamped to {}",
            requested_max_fps,
            max_configurable_max_fps);
    }
    if (requested_max_fps <= 0.0f) {
        xlog::warn(
            "max_fps=0 configured (uncapped). Some RF2 systems are frame-bound and may behave differently at very high FPS.");
    }
}

bool frame_limiter_is_active()
{
    return g_experimental_fps_stabilization_enabled;
}

bool frame_limiter_is_vsync_enabled()
{
    return g_vsync_enabled;
}

void frame_limiter_on_device_reset()
{
    if (!g_experimental_fps_stabilization_enabled) {
        return;
    }
    apply_frametime_limits(false);
    reset_present_limiter_state();
}

void frame_limiter_on_present()
{
    if (!g_experimental_fps_stabilization_enabled) {
        return;
    }
    frame_limiter_apply_runtime_overrides();
    enforce_present_fps_cap();
    update_fps_metrics();
}

void frame_limiter_draw_overlay(HWND target_window)
{
    if (!g_show_fps_overlay || !target_window || !IsWindow(target_window)) {
        return;
    }

    RECT client_rect{};
    if (!GetClientRect(target_window, &client_rect)) {
        return;
    }

    const int client_w = client_rect.right - client_rect.left;
    const int client_h = client_rect.bottom - client_rect.top;
    if (client_w <= 0 || client_h <= 0) {
        return;
    }

    HDC dc = GetDC(target_window);
    if (!dc) {
        return;
    }

    const int saved = SaveDC(dc);
    ensure_overlay_font();
    if (g_overlay_font) {
        SelectObject(dc, g_overlay_font);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fps_overlay_color);

    char text[128] = {};
    std::snprintf(
        text,
        sizeof(text),
        "sim: %.1f\ndraw: %.1f",
        std::max(g_sim_fps, 0.0f),
        std::max(g_draw_fps, 0.0f));

    RECT text_rect{
        std::max(0, client_w - fps_overlay_width_px - fps_overlay_margin_px),
        fps_overlay_margin_px,
        std::max(0, client_w - fps_overlay_margin_px),
        fps_overlay_margin_px + fps_overlay_height_px,
    };
    DrawTextA(dc, text, -1, &text_rect, DT_RIGHT | DT_TOP | DT_NOPREFIX | DT_NOCLIP);

    RestoreDC(dc, saved);
    ReleaseDC(target_window, dc);
}

bool frame_limiter_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines)
{
    out_success = false;
    out_status.clear();
    out_output_lines.clear();

    const std::string trimmed = trim_ascii_copy(command);
    const bool is_showfps_command = starts_with_case_insensitive(trimmed, "r_showfps");
    const bool is_maxfps_command = starts_with_case_insensitive(trimmed, "maxfps");

    if (!g_experimental_fps_stabilization_enabled && (is_showfps_command || is_maxfps_command)) {
        out_output_lines.emplace_back("Experimental FPS stabilization is disabled.");
        out_output_lines.emplace_back("Enable sopot_settings.ini option: experimental_fps_stabilization=1");
        out_status = "FPS stabilization command unavailable while experimental option is disabled.";
        return true;
    }

    if (is_showfps_command) {
        const std::string arg_text = trim_ascii_copy(trimmed.substr(9));
        if (arg_text.empty()) {
            char line[96] = {};
            std::snprintf(line, sizeof(line), "r_showfps is %d.", g_show_fps_overlay ? 1 : 0);
            out_output_lines.emplace_back(line);
            out_status = "Printed r_showfps state.";
            out_success = true;
            return true;
        }

        bool requested = false;
        if (!parse_bool_like(arg_text, requested)) {
            out_output_lines.emplace_back("Usage: r_showfps <0|1>");
            out_status = "Invalid r_showfps value.";
            return true;
        }

        g_show_fps_overlay = requested;
        save_showfps_to_settings();
        char line[96] = {};
        std::snprintf(line, sizeof(line), "r_showfps set to %d.", g_show_fps_overlay ? 1 : 0);
        out_output_lines.emplace_back(line);
        out_status = "Applied r_showfps.";
        out_success = true;
        return true;
    }

    if (!is_maxfps_command) {
        return false;
    }

    float value = 0.0f;
    if (!parse_max_fps_command_value(trimmed, value)) {
        out_output_lines.push_back("Usage: maxfps <num>");
        out_output_lines.push_back("Use maxfps 0 for uncapped.");
        out_output_lines.push_back("RF2 render cap is enforced in Present; very high values may expose frame-bound systems.");
        out_status = "Invalid maxfps value.";
        return true;
    }

    if (trimmed.size() == 6) {
        char line[192] = {};
        if (g_max_fps <= 0.0f) {
            std::snprintf(line, sizeof(line), "maxfps is uncapped (requested=0).");
        }
        else {
            std::snprintf(line, sizeof(line), "maxfps is %.2f.", get_effective_max_fps());
        }
        out_output_lines.emplace_back(line);
        out_status = "Printed maxfps state.";
        out_success = true;
        return true;
    }

    if (value < 0.0f) {
        out_output_lines.push_back("maxfps must be >= 0.");
        out_status = "Invalid maxfps value.";
        return true;
    }

    const float requested = value;
    g_max_fps = clamp_max_fps(value);
    apply_frametime_limits(true);
    save_max_fps_to_settings();

    if (g_max_fps <= 0.0f) {
        out_output_lines.emplace_back("maxfps uncapped. RF2 may run gameplay faster at very high FPS.");
    }
    else {
        char line[128] = {};
        std::snprintf(line, sizeof(line), "maxfps set to %.2f.", get_effective_max_fps());
        out_output_lines.emplace_back(line);
    }
    if (requested > max_configurable_max_fps) {
        char clamp_line[160] = {};
        std::snprintf(
            clamp_line,
            sizeof(clamp_line),
            "Requested %.2f was clamped to %.2f.",
            requested,
            max_configurable_max_fps);
        out_output_lines.emplace_back(clamp_line);
    }
    out_status = "Applied maxfps.";
    out_success = true;
    return true;
}
