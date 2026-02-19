#include "console.h"
#include "frame_limiter.h"
#include "../misc/misc.h"
#include "../player/camera.h"
#include "../rf2/os/console.h"
#include "../rf2/os/input.h"
#include "../rf2/rf2.h"
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <windows.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int console_open_height_px = 320;
constexpr int console_anim_step_px = 48;
constexpr COLORREF console_text_color = RGB(96, 255, 128);
constexpr COLORREF console_status_color = RGB(176, 224, 176);
constexpr COLORREF console_hint_color = RGB(128, 176, 128);
constexpr COLORREF console_border_color = RGB(64, 128, 64);
constexpr const char* console_help_text =
    "Input: Enter execute, Tab complete, . <text> search, fov/maxfps/r_showfps/ms/directinput/aimslow/enemycrosshair, ~ toggle";
constexpr size_t max_console_input_chars = 512;

template<size_t N>
uintptr_t find_pattern(const uint8_t* base, size_t size, const std::array<int, N>& pattern)
{
    if (size < N) {
        return 0;
    }

    for (size_t i = 0; i <= size - N; ++i) {
        bool matched = true;
        for (size_t j = 0; j < N; ++j) {
            int expected = pattern[j];
            if (expected >= 0 && base[i + j] != static_cast<uint8_t>(expected)) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return reinterpret_cast<uintptr_t>(base + i);
        }
    }
    return 0;
}

using ExecuteConsoleCommandFn = int(__cdecl*)(char*);
using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

struct ConsoleCommandInfo
{
    std::string name;
    std::string description;
};

void append_rf2patch_builtin_commands(std::vector<ConsoleCommandInfo>& commands)
{
    commands.push_back({"fov", "fov <num> (0 = auto-scale from 90 at 4:3)"});
    commands.push_back({"maxfps", "maxfps <num> (experimental; 0 = uncapped; render cap applied in Present)"});
    commands.push_back({"r_showfps", "r_showfps <0|1> (experimental; draw simulation/render FPS in top-right overlay)"});
    commands.push_back({"ms", "ms <num> (set gameplay mouse aim sensitivity; no arg prints current value)"});
    commands.push_back({"directinput", "directinput <0|1> (toggle DirectInput mouse for menus/gameplay)"});
    commands.push_back({"aimslow", "aimslow <0|1> (toggle target-on-enemy aim slowdown)"});
    commands.push_back({"enemycrosshair", "enemycrosshair <0|1> (toggle enemy crosshair variant image)"});
}

HWND g_console_hooked_game_window = nullptr;
WndProcFn g_original_game_window_proc = nullptr;
int g_console_command_log_count = 0;
int g_console_print_log_count = 0;
std::vector<std::string> g_console_output_lines{};
constexpr size_t max_console_output_lines = 300;
int g_console_refresh_suspension = 0;
bool g_console_refresh_pending = false;
bool g_console_is_open = false;
int g_console_current_height = 0;
int g_console_target_height = 0;
int g_console_scroll_lines_from_bottom = 0;
int g_console_visible_line_count = 12;
std::string g_console_input_text{};
std::string g_console_status_text{"Ready."};
HBRUSH g_console_bg_brush = nullptr;
HBRUSH g_console_border_brush = nullptr;
HFONT g_console_font = nullptr;
std::string g_tab_completion_seed{};
std::vector<std::string> g_tab_completion_matches{};
size_t g_tab_completion_index = 0;
HWND g_known_game_window = nullptr;

LRESULT CALLBACK game_window_proc_hook(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
void install_game_window_proc_hook(HWND window);
void __cdecl console_print_hook(const char* text, int channel);

FunHook<void __cdecl(const char*, int)> g_console_print_hook{
    static_cast<uintptr_t>(0),
    console_print_hook,
};

bool is_current_process_window(HWND window)
{
    if (!window || !IsWindow(window)) {
        return false;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    return process_id == GetCurrentProcessId();
}

bool is_current_process_top_level_window(HWND window)
{
    if (!is_current_process_window(window)) {
        return false;
    }
    if (GetAncestor(window, GA_ROOT) != window) {
        return false;
    }
    return true;
}

HWND resolve_top_level_window(HWND window)
{
    if (!is_current_process_window(window)) {
        return nullptr;
    }
    HWND root = GetAncestor(window, GA_ROOT);
    if (root && is_current_process_window(root) && IsWindow(root)) {
        return root;
    }
    return nullptr;
}

struct BestWindowSearch
{
    HWND window = nullptr;
    LONG area = -1;
};

BOOL CALLBACK enum_windows_find_best_cb(HWND hwnd, LPARAM l_param)
{
    auto* search = reinterpret_cast<BestWindowSearch*>(l_param);
    if (!is_current_process_top_level_window(hwnd)) {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }

    LONG width = std::max<LONG>(rect.right - rect.left, 0);
    LONG height = std::max<LONG>(rect.bottom - rect.top, 0);
    LONG area = width * height;
    if (area > search->area) {
        search->area = area;
        search->window = hwnd;
    }
    return TRUE;
}

HWND find_best_process_window()
{
    BestWindowSearch search{};
    EnumWindows(enum_windows_find_best_cb, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

HWND resolve_target_window(HWND window)
{
    HWND root = resolve_top_level_window(window);
    if (root) {
        return root;
    }
    if (is_current_process_top_level_window(g_known_game_window)) {
        return g_known_game_window;
    }
    return find_best_process_window();
}

std::string trim_ascii_copy(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string to_lower_ascii_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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

bool equals_case_insensitive(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && starts_with_case_insensitive(left, right);
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    const std::string haystack_lc = to_lower_ascii_copy(std::string{haystack});
    const std::string needle_lc = to_lower_ascii_copy(std::string{needle});
    return haystack_lc.find(needle_lc) != std::string::npos;
}

int get_control_profile_count()
{
    const int count = rf2::os::input::control_profile_count;
    if (count <= 0) {
        return 0;
    }
    return std::min(count, rf2::os::input::max_control_profiles);
}

bool is_valid_control_profile_index(int profile_index, int profile_count)
{
    return profile_index >= 0 && profile_index < profile_count;
}

uintptr_t get_control_profile_addr(int profile_index)
{
    return rf2::os::input::control_profiles_addr
        + static_cast<uintptr_t>(profile_index) * rf2::os::input::control_profile_size;
}

bool try_get_control_profile_look_sensitivity(int profile_index, float& out_x, float& out_y)
{
    const int profile_count = get_control_profile_count();
    if (!is_valid_control_profile_index(profile_index, profile_count)) {
        return false;
    }

    const uintptr_t profile_addr = get_control_profile_addr(profile_index);
    const float look_x =
        addr_as_ref<float>(profile_addr + rf2::os::input::control_profile_look_horizontal_sensitivity_offset);
    const float look_y =
        addr_as_ref<float>(profile_addr + rf2::os::input::control_profile_look_vertical_sensitivity_offset);
    if (!std::isfinite(look_x) || !std::isfinite(look_y)) {
        return false;
    }

    out_x = look_x;
    out_y = look_y;
    return true;
}

int get_preferred_control_profile_index()
{
    const int profile_count = get_control_profile_count();
    if (profile_count <= 0) {
        return -1;
    }

    const int active_profile = rf2::os::input::active_control_profile;
    if (is_valid_control_profile_index(active_profile, profile_count)) {
        return active_profile;
    }

    const int active_mouse_profile = rf2::os::input::active_mouse_control_profile;
    if (is_valid_control_profile_index(active_mouse_profile, profile_count)) {
        return active_mouse_profile;
    }

    return 0;
}

bool is_nonzero_sensitivity(float x, float y)
{
    constexpr float epsilon = 0.0000001f;
    return std::fabs(x) > epsilon || std::fabs(y) > epsilon;
}

bool try_get_mouse_aim_sensitivity(float& out_x, float& out_y)
{
    float patch_scale = 0.0f;
    if (misc_get_mouse_aim_sensitivity(patch_scale)) {
        out_x = patch_scale;
        out_y = patch_scale;
        return true;
    }

    bool have_candidate = false;
    float candidate_x = 0.0f;
    float candidate_y = 0.0f;

    const int preferred_profile = get_preferred_control_profile_index();
    if (preferred_profile >= 0) {
        float look_x = 0.0f;
        float look_y = 0.0f;
        if (try_get_control_profile_look_sensitivity(preferred_profile, look_x, look_y)) {
            candidate_x = look_x;
            candidate_y = look_y;
            have_candidate = true;
            if (is_nonzero_sensitivity(look_x, look_y)) {
                out_x = look_x;
                out_y = look_y;
                return true;
            }
        }
    }

    const int profile_count = get_control_profile_count();
    for (int i = 0; i < profile_count; ++i) {
        if (i == preferred_profile) {
            continue;
        }
        float look_x = 0.0f;
        float look_y = 0.0f;
        if (!try_get_control_profile_look_sensitivity(i, look_x, look_y)) {
            continue;
        }
        if (!have_candidate) {
            candidate_x = look_x;
            candidate_y = look_y;
            have_candidate = true;
        }
        if (is_nonzero_sensitivity(look_x, look_y)) {
            out_x = look_x;
            out_y = look_y;
            return true;
        }
    }

    // Secondary source: legacy gameplay aim sensitivity globals.
    const float aim_x = rf2::os::input::mouse_aim_sensitivity_x; // flt_6F2038
    const float aim_y = rf2::os::input::mouse_aim_sensitivity_y; // flt_6F2034
    if (std::isfinite(aim_x) && std::isfinite(aim_y)) {
        if (!have_candidate) {
            candidate_x = aim_x;
            candidate_y = aim_y;
            have_candidate = true;
        }
        if (is_nonzero_sensitivity(aim_x, aim_y)) {
            out_x = aim_x;
            out_y = aim_y;
            return true;
        }
    }

    // Fallback: derive from runtime DirectInput mouse scaling path.
    if (rf2::os::input::mouse_system_initialized != 0) {
        const int range_x = rf2::os::input::mouse_range_x;
        const int range_y = rf2::os::input::mouse_range_y;
        const float raw_x = rf2::os::input::mouse_sensitivity_x;
        const float raw_y = rf2::os::input::mouse_sensitivity_y;
        if (range_x > 0 && range_y > 0 && std::isfinite(raw_x) && std::isfinite(raw_y)) {
            constexpr float x_scale = 0.0015f;
            constexpr float y_scale = 0.0020000001f;
            const float arg0 = raw_x / (static_cast<float>(range_x) * x_scale);
            const float arg1 = raw_y / (static_cast<float>(range_y) * y_scale);
            if (std::isfinite(arg0) && std::isfinite(arg1)) {
                // Inverse of RF2 controls formula used before sub_539AE0:
                // arg0 = ((aim_y / 0.1) + 1) * 0.5
                // arg1 = ((aim_x / 0.07) + 1) * 0.5
                out_y = (arg0 * 2.0f - 1.0f) * 0.1f;
                out_x = (arg1 * 2.0f - 1.0f) * 0.07f;
                return std::isfinite(out_x) && std::isfinite(out_y);
            }
        }
    }

    if (have_candidate) {
        out_x = candidate_x;
        out_y = candidate_y;
        return true;
    }
    return false;
}

bool apply_control_profile_look_sensitivity(float look_x, float look_y)
{
    if (!std::isfinite(look_x) || !std::isfinite(look_y)) {
        return false;
    }

    const int profile_count = get_control_profile_count();
    bool wrote_any = false;
    for (int i = 0; i < profile_count; ++i) {
        const uintptr_t profile_addr = get_control_profile_addr(i);
        addr_as_ref<float>(profile_addr + rf2::os::input::control_profile_look_horizontal_sensitivity_offset) = look_x;
        addr_as_ref<float>(profile_addr + rf2::os::input::control_profile_look_vertical_sensitivity_offset) = look_y;
        wrote_any = true;
    }
    return wrote_any;
}

void apply_runtime_mouse_sensitivity(float aim_x, float aim_y)
{
    if (!std::isfinite(aim_x) || !std::isfinite(aim_y)) {
        return;
    }

    if (rf2::os::input::mouse_system_initialized == 0) {
        return;
    }

    // Match RF2 controls code path:
    // arg0 = ((aim_y / 0.1) + 1) * 0.5
    // arg1 = ((aim_x / 0.07) + 1) * 0.5
    constexpr float aim_x_divisor = 0.07f;
    constexpr float aim_y_divisor = 0.1f;
    const float arg0 = ((aim_y / aim_y_divisor) + 1.0f) * 0.5f;
    const float arg1 = ((aim_x / aim_x_divisor) + 1.0f) * 0.5f;
    if (!std::isfinite(arg0) || !std::isfinite(arg1)) {
        return;
    }
    rf2::os::input::mouse_set_sensitivity(arg0, arg1);
}

bool try_parse_positive_float(const std::string& text, float& out_value)
{
    size_t parsed_len = 0;
    try {
        out_value = std::stof(text, &parsed_len);
    }
    catch (...) {
        return false;
    }

    if (parsed_len != text.size() || !std::isfinite(out_value) || out_value < 0.0f) {
        return false;
    }
    return true;
}

void set_console_status_text(const char* text)
{
    g_console_status_text = text ? text : "";
}

void ensure_console_visual_resources()
{
    if (!g_console_bg_brush) {
        g_console_bg_brush = CreateSolidBrush(RGB(0, 0, 0));
    }
    if (!g_console_border_brush) {
        g_console_border_brush = CreateSolidBrush(console_border_color);
    }
    if (!g_console_font) {
        g_console_font = CreateFontA(
            -17,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            ANSI_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN,
            "Consolas");
        if (!g_console_font) {
            g_console_font = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
        }
    }
}

int get_max_scroll_lines()
{
    const int total_lines = static_cast<int>(g_console_output_lines.size());
    return std::max(0, total_lines - g_console_visible_line_count);
}

void clamp_console_scroll()
{
    g_console_scroll_lines_from_bottom = std::clamp(
        g_console_scroll_lines_from_bottom,
        0,
        get_max_scroll_lines());
}

void refresh_console_output_view()
{
    clamp_console_scroll();
}

void suspend_console_output_refresh()
{
    ++g_console_refresh_suspension;
}

void resume_console_output_refresh()
{
    if (g_console_refresh_suspension <= 0) {
        return;
    }
    --g_console_refresh_suspension;
    if (g_console_refresh_suspension == 0 && g_console_refresh_pending) {
        g_console_refresh_pending = false;
        refresh_console_output_view();
    }
}

void refresh_console_output_if_needed()
{
    if (g_console_refresh_suspension > 0) {
        g_console_refresh_pending = true;
        return;
    }
    refresh_console_output_view();
}

void scroll_console_output_to_top()
{
    g_console_scroll_lines_from_bottom = get_max_scroll_lines();
}

void scroll_console_output_to_bottom()
{
    g_console_scroll_lines_from_bottom = 0;
}

void scroll_console_output_by(WPARAM scroll_cmd)
{
    const int step = std::max(1, g_console_visible_line_count - 2);
    switch (scroll_cmd) {
    case SB_PAGEUP:
    case SB_LINEUP:
        g_console_scroll_lines_from_bottom += step;
        break;
    case SB_PAGEDOWN:
    case SB_LINEDOWN:
        g_console_scroll_lines_from_bottom -= step;
        break;
    default:
        break;
    }
    clamp_console_scroll();
}

void append_console_output_line(std::string line)
{
    line = trim_ascii_copy(std::move(line));
    if (line.empty()) {
        return;
    }

    if (g_console_scroll_lines_from_bottom > 0) {
        ++g_console_scroll_lines_from_bottom;
    }

    g_console_output_lines.push_back(std::move(line));
    if (g_console_output_lines.size() > max_console_output_lines) {
        const size_t overflow = g_console_output_lines.size() - max_console_output_lines;
        g_console_output_lines.erase(
            g_console_output_lines.begin(),
            g_console_output_lines.begin() + static_cast<std::ptrdiff_t>(overflow));
    }

    refresh_console_output_if_needed();
}

void append_console_output_text(const char* text)
{
    if (!text || !*text) {
        return;
    }

    suspend_console_output_refresh();
    std::string current;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            append_console_output_line(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    append_console_output_line(current);
    resume_console_output_refresh();
}

bool collect_rf2_console_commands(std::vector<ConsoleCommandInfo>& out_commands)
{
    out_commands.clear();

    const int count_raw = rf2::os::console::command_count;
    const int count = std::clamp(count_raw, 0, rf2::os::console::max_commands);
    auto** table = rf2::os::console::command_table_entries();
    if (!table || count <= 0) {
        return false;
    }

    out_commands.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto* entry = table[i];
        if (!entry || !entry->name || entry->name[0] == '\0') {
            continue;
        }

        ConsoleCommandInfo info{};
        info.name = entry->name;
        if (entry->description && entry->description[0] != '\0') {
            info.description = entry->description;
        }
        out_commands.push_back(std::move(info));
    }
    return !out_commands.empty();
}

bool collect_all_console_commands(std::vector<ConsoleCommandInfo>& out_commands)
{
    std::vector<ConsoleCommandInfo> stock_commands;
    collect_rf2_console_commands(stock_commands);
    out_commands = std::move(stock_commands);
    append_rf2patch_builtin_commands(out_commands);
    return !out_commands.empty();
}

bool parse_search_command_request(const std::string& command, std::string& out_needle)
{
    const std::string trimmed = trim_ascii_copy(command);
    if (trimmed.empty() || trimmed.front() != '.') {
        return false;
    }

    out_needle = trim_ascii_copy(trimmed.substr(1));
    return true;
}

bool print_rf2_command_search(const std::string& needle)
{
    std::vector<ConsoleCommandInfo> commands;
    if (!collect_all_console_commands(commands)) {
        append_console_output_line("No console commands are currently available.");
        return false;
    }

    suspend_console_output_refresh();
    append_console_output_line("Command search for: " + needle);
    int printed = 0;
    for (const auto& cmd : commands) {
        if (!contains_case_insensitive(cmd.name, needle)) {
            continue;
        }
        std::string line = cmd.name;
        if (!cmd.description.empty()) {
            line += " - ";
            line += cmd.description;
        }
        append_console_output_line(line);
        ++printed;
    }

    char summary[120] = {};
    if (printed == 0) {
        std::snprintf(summary, sizeof(summary), "No commands contain \"%s\".", needle.c_str());
        append_console_output_line(summary);
        resume_console_output_refresh();
        return false;
    }

    std::snprintf(summary, sizeof(summary), "Found %d matching commands.", printed);
    append_console_output_line(summary);
    resume_console_output_refresh();
    return true;
}

void reset_tab_completion_state()
{
    g_tab_completion_seed.clear();
    g_tab_completion_matches.clear();
    g_tab_completion_index = 0;
}

bool apply_console_edit_text(const std::string& text)
{
    g_console_input_text = text.substr(0, max_console_input_chars);
    return true;
}

bool tab_complete_console_input()
{
    const std::string current = trim_ascii_copy(g_console_input_text);
    if (current.empty()) {
        set_console_status_text("Type part of a command, then press Tab.");
        reset_tab_completion_state();
        return false;
    }
    if (current.find_first_of(" \t") != std::string::npos) {
        set_console_status_text("Tab completion only applies to the command name.");
        reset_tab_completion_state();
        return false;
    }

    bool current_is_match = false;
    size_t current_match_index = 0;
    for (size_t i = 0; i < g_tab_completion_matches.size(); ++i) {
        if (equals_case_insensitive(g_tab_completion_matches[i], current)) {
            current_is_match = true;
            current_match_index = i;
            break;
        }
    }

    bool rebuild_matches = g_tab_completion_matches.empty();
    if (!rebuild_matches) {
        if (!equals_case_insensitive(current, g_tab_completion_seed) && !current_is_match) {
            rebuild_matches = true;
        }
    }

    if (rebuild_matches) {
        std::vector<ConsoleCommandInfo> commands;
        if (!collect_all_console_commands(commands)) {
            set_console_status_text("No console commands available.");
            reset_tab_completion_state();
            return false;
        }

        g_tab_completion_matches.clear();
        for (const auto& cmd : commands) {
            if (starts_with_case_insensitive(cmd.name, current)) {
                g_tab_completion_matches.push_back(cmd.name);
            }
        }

        g_tab_completion_seed = current;
        g_tab_completion_index = 0;
    }
    else {
        if (current_is_match) {
            g_tab_completion_index = (current_match_index + 1) % g_tab_completion_matches.size();
        }
        else {
            g_tab_completion_index = (g_tab_completion_index + 1) % g_tab_completion_matches.size();
        }
    }

    if (g_tab_completion_matches.empty()) {
        set_console_status_text("No command matches.");
        reset_tab_completion_state();
        return false;
    }

    const std::string& completion = g_tab_completion_matches[g_tab_completion_index];
    apply_console_edit_text(completion);

    char status[120] = {};
    std::snprintf(
        status,
        sizeof(status),
        "Tab completion %zu/%zu: %s",
        g_tab_completion_index + 1,
        g_tab_completion_matches.size(),
        completion.c_str());
    set_console_status_text(status);
    return true;
}

bool is_help_command_request(const std::string& command)
{
    const std::string trimmed = trim_ascii_copy(command);
    if (trimmed.empty()) {
        return false;
    }

    const std::string lowered = to_lower_ascii_copy(trimmed);
    if (lowered == "help" || lowered == "?" || lowered == "commands") {
        return true;
    }
    return false;
}

bool print_rf2_command_help()
{
    std::vector<ConsoleCommandInfo> stock_commands;
    collect_rf2_console_commands(stock_commands);

    std::vector<ConsoleCommandInfo> rf2patch_commands;
    append_rf2patch_builtin_commands(rf2patch_commands);

    suspend_console_output_refresh();
    append_console_output_line("Stock RF2 commands:");
    int stock_printed = 0;
    for (const auto& cmd : stock_commands) {
        std::string line = cmd.name;
        if (!cmd.description.empty()) {
            line += " - ";
            line += cmd.description;
        }
        append_console_output_line(line);
        ++stock_printed;
    }
    if (stock_printed == 0) {
        append_console_output_line("(none detected)");
    }

    append_console_output_line("SOPOT commands:");
    int rf2patch_printed = 0;
    for (const auto& cmd : rf2patch_commands) {
        std::string line = cmd.name;
        if (!cmd.description.empty()) {
            line += " - ";
            line += cmd.description;
        }
        append_console_output_line(line);
        ++rf2patch_printed;
    }

    if (stock_printed == 0 && rf2patch_printed == 0) {
        append_console_output_line("No command names were found.");
        resume_console_output_refresh();
        return false;
    }

    char summary[128] = {};
    std::snprintf(
        summary,
        sizeof(summary),
        "Printed %d stock commands and %d SOPOT commands.",
        stock_printed,
        rf2patch_printed);
    append_console_output_line(summary);
    resume_console_output_refresh();
    return true;
}

LRESULT call_original_game_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    if (g_original_game_window_proc) {
        return CallWindowProcA(g_original_game_window_proc, hwnd, msg, w_param, l_param);
    }
    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

uintptr_t find_console_print_target()
{
    static constexpr std::array<int, 27> pattern = {
        0x68, -1, -1, -1, -1,     // push offset byte_B62FA0
        0xE8, -1, -1, -1, -1,     // call _sprintf
        0x83, 0xC4, 0x10,         // add esp, 10h
        0x53,                     // push ebx (0)
        0x68, -1, -1, -1, -1,     // push offset byte_B62FA0
        0xE8, -1, -1, -1, -1,     // call nullsub_112
        0x83, 0xC4, 0x14,         // add esp, 14h
    };
    static constexpr size_t call_opcode_index = 19;

    auto* module_base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    if (!module_base) {
        return 0;
    }

    auto* dos_hdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt_hdr = reinterpret_cast<const IMAGE_NT_HEADERS*>(module_base + dos_hdr->e_lfanew);
    if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    uintptr_t match_addr = find_pattern(module_base, nt_hdr->OptionalHeader.SizeOfImage, pattern);
    if (!match_addr) {
        return 0;
    }

    const uintptr_t call_instr = match_addr + call_opcode_index;
    const int32_t rel = addr_as_ref<int32_t>(call_instr + 1);
    const uintptr_t target = call_instr + 5 + rel;
    const uintptr_t module_start = reinterpret_cast<uintptr_t>(module_base);
    const uintptr_t module_end = module_start + nt_hdr->OptionalHeader.SizeOfImage;
    if (target < module_start || target >= module_end) {
        return 0;
    }

    // IDA marks nullsub_112 as a 1-byte function.
    if (addr_as_ref<uint8_t>(target) != 0xC3) {
        return 0;
    }
    return target;
}

void install_console_print_hook()
{
    static bool installed = false;
    if (installed) {
        return;
    }

    const uintptr_t print_addr = find_console_print_target();
    if (!print_addr) {
        xlog::warn("Could not locate RF2 console print sink (nullsub_112)");
        installed = true;
        return;
    }

    g_console_print_hook.set_addr(print_addr);
    g_console_print_hook.install();
    installed = true;
    xlog::info("Installed RF2 console output hook at 0x{:X}", static_cast<unsigned>(print_addr));
}

bool rf2_console_command_api_available()
{
    const uintptr_t base = rf2::module_base();
    if (!base) {
        return false;
    }

    auto* fn_bytes = reinterpret_cast<const uint8_t*>(base + rf2::os::console::execute_command_rva);
    return fn_bytes[0] == 0x55 && fn_bytes[1] == 0x8B && fn_bytes[2] == 0xEC;
}

int execute_rf2_console_command(const std::string& raw_command)
{
    const std::string command = trim_ascii_copy(raw_command);
    if (command.empty()) {
        return -1;
    }

    if (!rf2_console_command_api_available()) {
        xlog::warn("RF2 console command API signature mismatch; command execution unavailable");
        return -2;
    }

    auto execute_fn = rf2::os::console::execute_command_ptr();

    std::vector<char> command_buf(command.begin(), command.end());
    command_buf.push_back('\0');
    const int result = execute_fn(command_buf.data());

    if (g_console_command_log_count < 32) {
        ++g_console_command_log_count;
        xlog::info("RF2 console command: \"{}\" -> {}", command, result);
    }
    return result;
}

void __cdecl console_print_hook(const char* text, int channel)
{
    (void)channel;
    append_console_output_text(text);
    if (g_console_print_log_count < 48 && text && *text) {
        ++g_console_print_log_count;
        xlog::info("RF2 console output: {}", text);
    }
    g_console_print_hook.call_target(text, channel);
}

void run_console_command_from_ui()
{
    const std::string command = trim_ascii_copy(g_console_input_text);
    if (command.empty()) {
        set_console_status_text("Enter a command.");
        return;
    }

    reset_tab_completion_state();
    append_console_output_line("> " + command);

    if (starts_with_case_insensitive(command, "ms")) {
        const bool has_token_boundary =
            (command.size() == 2)
            || std::isspace(static_cast<unsigned char>(command[2])) != 0;
        if (has_token_boundary) {
            const std::string arg_text = trim_ascii_copy(command.substr(2));
            if (arg_text.empty()) {
                float aim_x = 0.0f;
                float aim_y = 0.0f;
                if (try_get_mouse_aim_sensitivity(aim_x, aim_y)) {
                    char line[160] = {};
                    if (std::fabs(aim_x - aim_y) < 0.0001f) {
                        std::snprintf(line, sizeof(line), "mouse sensitivity = %.6g", aim_x);
                    }
                    else {
                        std::snprintf(line, sizeof(line), "mouse sensitivity x=%.6g y=%.6g", aim_x, aim_y);
                    }
                    append_console_output_line(line);
                    set_console_status_text("Printed current sensitivity.");
                }
                else {
                    append_console_output_line("Could not query gameplay mouse sensitivity.");
                    set_console_status_text("Sensitivity query failed.");
                }
                g_console_input_text.clear();
                return;
            }

            float value = 0.0f;
            if (!try_parse_positive_float(arg_text, value)) {
                append_console_output_line("Usage: ms <num>");
                set_console_status_text("Invalid sensitivity value.");
                g_console_input_text.clear();
                return;
            }

            const bool updated_directinput_scale = misc_set_mouse_aim_sensitivity(value);
            const bool updated_profile_table = apply_control_profile_look_sensitivity(value, value);
            rf2::os::input::mouse_aim_sensitivity_x = value;
            rf2::os::input::mouse_aim_sensitivity_y = value;
            {
                char line[128] = {};
                std::snprintf(line, sizeof(line), "gameplay mouse sensitivity set to %.6g", value);
                append_console_output_line(line);
                if (updated_directinput_scale && rf2::os::input::mouse_system_initialized != 0) {
                    set_console_status_text("Applied sensitivity.");
                }
                else if (updated_directinput_scale) {
                    set_console_status_text("Applied gameplay sensitivity scale; runtime input not initialized.");
                }
                else if (updated_profile_table) {
                    set_console_status_text("Applied gameplay sensitivity; DirectInput runtime not initialized.");
                }
                else if (rf2::os::input::mouse_system_initialized != 0) {
                    set_console_status_text("Applied runtime sensitivity; gameplay profile table unavailable.");
                }
                else {
                    set_console_status_text("Sensitivity saved; gameplay/runtime input not initialized yet.");
                }
            }
            g_console_input_text.clear();
            return;
        }
    }

    std::string custom_status;
    std::vector<std::string> custom_lines;
    bool custom_success = false;
    if (frame_limiter_try_handle_console_command(command, custom_success, custom_status, custom_lines)
        || misc_try_handle_console_command(command, custom_success, custom_status, custom_lines)
        || camera_try_handle_console_command(command, custom_success, custom_status, custom_lines)) {
        for (const auto& line : custom_lines) {
            append_console_output_line(line);
        }
        if (!custom_status.empty()) {
            set_console_status_text(custom_status.c_str());
        }
        else if (!custom_success) {
            set_console_status_text("Custom command failed.");
        }
        g_console_input_text.clear();
        return;
    }

    std::string search_needle;
    if (parse_search_command_request(command, search_needle)) {
        if (search_needle.empty()) {
            append_console_output_line("Usage: . <string>");
            set_console_status_text("Usage: . <string>");
        }
        else if (print_rf2_command_search(search_needle)) {
            set_console_status_text("Printed command search results.");
        }
        else {
            set_console_status_text("No matching commands found.");
        }
        g_console_input_text.clear();
        return;
    }

    if (is_help_command_request(command)) {
        if (print_rf2_command_help()) {
            set_console_status_text("Printed RF2 command list.");
        }
        else {
            set_console_status_text("Could not print RF2 command list.");
        }
        g_console_input_text.clear();
        return;
    }

    const int result = execute_rf2_console_command(command);
    char status[160] = {};
    if (result == 0) {
        std::snprintf(status, sizeof(status), "Executed: %s", command.c_str());
    }
    else {
        std::snprintf(status, sizeof(status), "Command failed (code %d): %s", result, command.c_str());
    }
    set_console_status_text(status);
    g_console_input_text.clear();
}

void hide_console_window(bool restore_game_focus)
{
    (void)restore_game_focus;
    reset_tab_completion_state();
    g_console_is_open = false;
    g_console_target_height = 0;

    // Reset RF2 key state so gameplay controls do not bleed through while the SOPOT console is open/closing.
    rf2::os::input::reset_key_state();
    rf2::os::input::alt_key_down = 0;
    rf2::os::input::tab_key_down = 0;
}

void update_console_target_height()
{
    if (!g_console_hooked_game_window || !IsWindow(g_console_hooked_game_window)) {
        g_console_target_height = 0;
        return;
    }

    RECT parent_client{};
    GetClientRect(g_console_hooked_game_window, &parent_client);
    const int parent_height = std::max<int>(parent_client.bottom - parent_client.top, 1);
    const int max_open_height = std::max<int>(120, (parent_height * 4) / 5);
    g_console_target_height = g_console_is_open ? std::min<int>(console_open_height_px, max_open_height) : 0;
}

void step_console_animation()
{
    update_console_target_height();

    if (g_console_current_height < g_console_target_height) {
        g_console_current_height = std::min(g_console_current_height + console_anim_step_px, g_console_target_height);
    }
    else if (g_console_current_height > g_console_target_height) {
        g_console_current_height = std::max(g_console_current_height - console_anim_step_px, g_console_target_height);
    }
}

bool append_clipboard_text_to_input()
{
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    bool success = false;
    HANDLE clip = GetClipboardData(CF_TEXT);
    if (clip) {
        const char* text = static_cast<const char*>(GlobalLock(clip));
        if (text) {
            for (const char* p = text; *p; ++p) {
                const char c = *p;
                if (c == '\r' || c == '\n' || c == '\t') {
                    continue;
                }
                if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
                    continue;
                }
                if (g_console_input_text.size() >= max_console_input_chars) {
                    break;
                }
                g_console_input_text.push_back(c);
            }
            GlobalUnlock(clip);
            success = true;
        }
    }

    CloseClipboard();
    return success;
}

bool handle_console_keydown(WPARAM w_param)
{
    switch (w_param) {
    case VK_RETURN:
        run_console_command_from_ui();
        return true;

    case VK_TAB:
        tab_complete_console_input();
        return true;

    case VK_BACK:
        if (!g_console_input_text.empty()) {
            g_console_input_text.pop_back();
            reset_tab_completion_state();
        }
        return true;

    case VK_ESCAPE:
        hide_console_window(true);
        return true;

    case VK_PRIOR:
        scroll_console_output_by(SB_PAGEUP);
        return true;

    case VK_NEXT:
        scroll_console_output_by(SB_PAGEDOWN);
        return true;

    case VK_HOME:
        scroll_console_output_to_top();
        return true;

    case VK_END:
        scroll_console_output_to_bottom();
        return true;

    default:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (w_param == 'V' || w_param == 'v')) {
            append_clipboard_text_to_input();
            reset_tab_completion_state();
            return true;
        }
        else {
            reset_tab_completion_state();
        }
        return true;
    }
}

void handle_console_char(WPARAM w_param)
{
    if (w_param == '`' || w_param == '~') {
        return;
    }

    if (w_param < 32 || w_param > 126) {
        return;
    }

    if (g_console_input_text.size() >= max_console_input_chars) {
        return;
    }

    g_console_input_text.push_back(static_cast<char>(w_param));
    reset_tab_completion_state();
}

void draw_console_overlay(HWND target_window)
{
    if (!target_window || !IsWindow(target_window)) {
        return;
    }

    RECT client{};
    if (!GetClientRect(target_window, &client)) {
        return;
    }
    const int client_w = client.right - client.left;
    const int client_h = client.bottom - client.top;
    if (client_w <= 0 || client_h <= 0) {
        return;
    }

    const int panel_h = std::clamp(g_console_current_height, 0, client_h);
    if (panel_h <= 0) {
        return;
    }

    HDC dc = GetDC(target_window);
    if (!dc) {
        return;
    }

    ensure_console_visual_resources();

    const int saved_dc = SaveDC(dc);
    if (g_console_font) {
        SelectObject(dc, g_console_font);
    }
    SetBkMode(dc, TRANSPARENT);

    RECT panel_rect{0, 0, client_w, panel_h};
    FillRect(dc, &panel_rect, g_console_bg_brush ? g_console_bg_brush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    FrameRect(dc, &panel_rect, g_console_border_brush ? g_console_border_brush : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    const int line_h = std::max<int>(tm.tmHeight, 14);

    const int margin = 8;
    const int gap = 6;
    const int input_y = panel_h - margin - line_h;
    const int status_y = input_y - gap - line_h;
    const int help_y = status_y - gap - line_h;
    const int output_top = margin;
    const int output_bottom = std::max<int>(output_top, help_y - gap);

    RECT output_rect{margin, output_top, std::max<int>(margin, client_w - margin), output_bottom};
    if (output_rect.bottom > output_rect.top + 4) {
        FrameRect(dc, &output_rect, g_console_border_brush ? g_console_border_brush : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    SetTextColor(dc, console_hint_color);
    TextOutA(dc, margin, help_y, console_help_text, static_cast<int>(std::strlen(console_help_text)));

    SetTextColor(dc, console_status_color);
    TextOutA(dc, margin, status_y, g_console_status_text.c_str(), static_cast<int>(g_console_status_text.size()));

    int output_inner_h = std::max<int>(0, static_cast<int>(output_rect.bottom - output_rect.top) - 6);
    g_console_visible_line_count = std::max(1, output_inner_h / line_h);
    clamp_console_scroll();

    if (output_inner_h > 0) {
        const int saved_clip = SaveDC(dc);
        IntersectClipRect(dc, output_rect.left + 3, output_rect.top + 3, output_rect.right - 3, output_rect.bottom - 3);

        SetTextColor(dc, console_text_color);
        const int total_lines = static_cast<int>(g_console_output_lines.size());
        const int first_line = std::max(0, total_lines - g_console_visible_line_count - g_console_scroll_lines_from_bottom);
        for (int i = 0; i < g_console_visible_line_count; ++i) {
            const int idx = first_line + i;
            if (idx < 0 || idx >= total_lines) {
                continue;
            }
            const int y = output_rect.top + 3 + (i * line_h);
            const auto& line = g_console_output_lines[static_cast<size_t>(idx)];
            TextOutA(dc, output_rect.left + 6, y, line.c_str(), static_cast<int>(line.size()));
        }

        RestoreDC(dc, saved_clip);
    }

    SetTextColor(dc, console_text_color);
    std::string input_line = "> " + g_console_input_text;
    if (g_console_is_open) {
        input_line.push_back('_');
    }
    const int max_input_w = std::max(32, client_w - (margin * 2));
    while (!input_line.empty()) {
        SIZE text_size{};
        if (!GetTextExtentPoint32A(dc, input_line.c_str(), static_cast<int>(input_line.size()), &text_size) || text_size.cx <= max_input_w) {
            break;
        }
        input_line.erase(input_line.begin());
    }
    TextOutA(dc, margin, input_y, input_line.c_str(), static_cast<int>(input_line.size()));

    if (g_console_scroll_lines_from_bottom > 0) {
        char scroll_info[96] = {};
        std::snprintf(scroll_info, sizeof(scroll_info), "(%d lines up)", g_console_scroll_lines_from_bottom);
        SIZE text_size{};
        GetTextExtentPoint32A(dc, scroll_info, static_cast<int>(std::strlen(scroll_info)), &text_size);
        SetTextColor(dc, console_hint_color);
        TextOutA(
            dc,
            std::max<int>(margin, client_w - margin - static_cast<int>(text_size.cx)),
            margin + 1,
            scroll_info,
            static_cast<int>(std::strlen(scroll_info)));
    }

    GdiFlush();
    RestoreDC(dc, saved_dc);
    ReleaseDC(target_window, dc);
}

void toggle_console_window(HWND owner)
{
    owner = resolve_target_window(owner);
    if (!owner || !IsWindow(owner)) {
        return;
    }

    g_console_hooked_game_window = owner;
    g_console_is_open = !g_console_is_open;
    reset_tab_completion_state();
    if (g_console_is_open) {
        set_console_status_text("Ready. Enter RF2 command and press Enter.");
        scroll_console_output_to_bottom();
    }

    // Reset RF2 key state so gameplay controls do not bleed through while toggling console mode.
    rf2::os::input::reset_key_state();
    rf2::os::input::alt_key_down = 0;
    rf2::os::input::tab_key_down = 0;
}

LRESULT CALLBACK game_window_proc_hook(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (w_param == VK_OEM_3) {
            return 0;
        }
        if (g_console_is_open) {
            handle_console_keydown(w_param);
            return 0;
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (w_param == VK_OEM_3) {
            toggle_console_window(hwnd);
            return 0;
        }
        if (g_console_is_open) {
            return 0;
        }
        break;

    case WM_CHAR:
    case WM_SYSCHAR:
        if (w_param == '`' || w_param == '~') {
            return 0;
        }
        if (g_console_is_open) {
            handle_console_char(w_param);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        g_console_hooked_game_window = nullptr;
        g_console_current_height = 0;
        g_console_target_height = 0;
        g_console_is_open = false;
        g_console_input_text.clear();
        reset_tab_completion_state();
        break;

    default:
        break;
    }
    return call_original_game_window_proc(hwnd, msg, w_param, l_param);
}

void install_game_window_proc_hook(HWND window)
{
    window = resolve_target_window(window);
    if (!window || !IsWindow(window)) {
        return;
    }

    if (g_console_hooked_game_window == window && g_original_game_window_proc) {
        return;
    }

    if (g_console_hooked_game_window
        && g_original_game_window_proc
        && IsWindow(g_console_hooked_game_window))
    {
        SetWindowLongPtrA(
            g_console_hooked_game_window,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_original_game_window_proc));
        g_console_hooked_game_window = nullptr;
        g_original_game_window_proc = nullptr;
    }

    auto current_proc = reinterpret_cast<WndProcFn>(GetWindowLongPtrA(window, GWLP_WNDPROC));
    if (current_proc == game_window_proc_hook) {
        g_console_hooked_game_window = window;
        return;
    }

    if (!current_proc) {
        return;
    }

    auto previous_proc = SetWindowLongPtrA(
        window,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(game_window_proc_hook));
    if (!previous_proc) {
        return;
    }

    g_original_game_window_proc = reinterpret_cast<WndProcFn>(previous_proc);
    g_console_hooked_game_window = window;
}

} // namespace

void console_install_output_hook()
{
    install_console_print_hook();
}

void console_attach_to_window(HWND window)
{
    g_known_game_window = window;
    install_game_window_proc_hook(window);
}

bool console_is_open()
{
    return g_console_is_open;
}

void console_on_present(HWND target_window)
{
    HWND resolved = resolve_target_window(target_window ? target_window : g_console_hooked_game_window);
    if (!resolved || !IsWindow(resolved)) {
        return;
    }
    g_console_hooked_game_window = resolved;

    step_console_animation();
    if (!g_console_is_open && g_console_current_height <= 0) {
        return;
    }

    if (g_console_is_open) {
        // Keep RF2 key-state arrays from retaining gameplay input while console mode is active.
        rf2::os::input::reset_key_state();
        rf2::os::input::alt_key_down = 0;
        rf2::os::input::tab_key_down = 0;
    }

    draw_console_overlay(resolved);
}
