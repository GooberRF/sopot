#include "console.h"
#include "frame_limiter.h"
#include "../misc/misc.h"
#include "../player/camera.h"
#include "../rf2/os/console.h"
#include "../rf2/rf2.h"
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <windows.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr const char* rf2patch_console_class_name = "SopotConsoleWindow";
constexpr int id_console_label = 3100;
constexpr int id_console_edit = 3101;
constexpr int id_console_status = 3104;
constexpr int id_console_output = 3105;
constexpr UINT console_anim_timer_id = 0x52F2;
constexpr UINT console_anim_timer_interval_ms = 20;
constexpr int console_open_height_px = 320;
constexpr int console_anim_step_px = 96;
constexpr COLORREF console_text_color = RGB(96, 255, 128);
constexpr COLORREF console_status_color = RGB(176, 224, 176);

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
    commands.push_back({"maxfps", "maxfps <num> (0 = uncapped; render cap applied in Present)"});
    commands.push_back({"r_showfps", "r_showfps <0|1> (draw simulation/render FPS in top-right overlay)"});
    commands.push_back({"directinput", "directinput <0|1> (toggle DirectInput mouse for menus/gameplay)"});
    commands.push_back({"aimslow", "aimslow <0|1> (toggle target-on-enemy aim slowdown)"});
    commands.push_back({"enemycrosshair", "enemycrosshair <0|1> (toggle enemy crosshair variant image)"});
}

HWND g_console_window = nullptr;
HWND g_console_edit = nullptr;
HWND g_console_output = nullptr;
HWND g_console_status = nullptr;
HWND g_console_hooked_game_window = nullptr;
WndProcFn g_original_game_window_proc = nullptr;
WndProcFn g_original_console_edit_proc = nullptr;
int g_console_command_log_count = 0;
int g_console_print_log_count = 0;
std::vector<std::string> g_console_output_lines{};
constexpr size_t max_console_output_lines = 300;
int g_console_refresh_suspension = 0;
bool g_console_refresh_pending = false;
bool g_console_is_open = false;
int g_console_current_height = 0;
int g_console_target_height = 0;
HBRUSH g_console_bg_brush = nullptr;
HFONT g_console_font = nullptr;
std::string g_tab_completion_seed{};
std::vector<std::string> g_tab_completion_matches{};
size_t g_tab_completion_index = 0;
HWND g_known_game_window = nullptr;

LRESULT CALLBACK console_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
LRESULT CALLBACK console_edit_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
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

std::string get_window_text(HWND control)
{
    if (!control || !IsWindow(control)) {
        return {};
    }

    const int text_len = GetWindowTextLengthA(control);
    if (text_len <= 0) {
        return {};
    }

    std::string buffer(static_cast<size_t>(text_len) + 1, '\0');
    GetWindowTextA(control, buffer.data(), text_len + 1);
    buffer.resize(static_cast<size_t>(text_len));
    return buffer;
}

void set_console_status_text(const char* text)
{
    if (g_console_status && IsWindow(g_console_status)) {
        SetWindowTextA(g_console_status, text ? text : "");
    }
}

void ensure_console_visual_resources()
{
    if (!g_console_bg_brush) {
        g_console_bg_brush = CreateSolidBrush(RGB(0, 0, 0));
    }
    if (!g_console_font) {
        // Classic fixed-width console look.
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

void apply_console_window_skin(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    ensure_console_visual_resources();

    LONG ex_style = GetWindowLongA(hwnd, GWL_EXSTYLE);
    ex_style |= WS_EX_LAYERED;
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex_style);
    SetLayeredWindowAttributes(hwnd, 0, 224, LWA_ALPHA);
}

void refresh_console_output_view()
{
    if (!g_console_output || !IsWindow(g_console_output)) {
        return;
    }

    std::string joined;
    joined.reserve(g_console_output_lines.size() * 48);
    for (const auto& line : g_console_output_lines) {
        joined += line;
        joined += "\r\n";
    }
    SetWindowTextA(g_console_output, joined.c_str());
    // Keep viewport pinned to the bottom so newest entries are always visible.
    const LRESULT line_count = SendMessageA(g_console_output, EM_GETLINECOUNT, 0, 0);
    SendMessageA(g_console_output, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageA(g_console_output, EM_SCROLLCARET, 0, 0);
    SendMessageA(g_console_output, WM_VSCROLL, static_cast<WPARAM>(SB_BOTTOM), 0);
    if (line_count > 0) {
        SendMessageA(g_console_output, EM_LINESCROLL, 0, static_cast<LPARAM>(line_count));
    }
    SendMessageA(g_console_output, WM_VSCROLL, static_cast<WPARAM>(SB_BOTTOM), 0);
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
    if (!g_console_output || !IsWindow(g_console_output)) {
        return;
    }
    SendMessageA(g_console_output, EM_SETSEL, 0, 0);
    SendMessageA(g_console_output, EM_SCROLLCARET, 0, 0);
    SendMessageA(g_console_output, WM_VSCROLL, static_cast<WPARAM>(SB_TOP), 0);
}

void scroll_console_output_to_bottom()
{
    if (!g_console_output || !IsWindow(g_console_output)) {
        return;
    }
    SendMessageA(g_console_output, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageA(g_console_output, EM_SCROLLCARET, 0, 0);
    SendMessageA(g_console_output, WM_VSCROLL, static_cast<WPARAM>(SB_BOTTOM), 0);
}

void scroll_console_output_by(WPARAM scroll_cmd)
{
    if (!g_console_output || !IsWindow(g_console_output)) {
        return;
    }
    SendMessageA(g_console_output, WM_VSCROLL, scroll_cmd, 0);
}

void append_console_output_line(std::string line)
{
    line = trim_ascii_copy(std::move(line));
    if (line.empty()) {
        return;
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
    if (!g_console_edit || !IsWindow(g_console_edit)) {
        return false;
    }
    SetWindowTextA(g_console_edit, text.c_str());
    SendMessageA(
        g_console_edit,
        EM_SETSEL,
        static_cast<WPARAM>(text.size()),
        static_cast<LPARAM>(text.size()));
    return true;
}

bool tab_complete_console_input()
{
    if (!g_console_edit || !IsWindow(g_console_edit)) {
        return false;
    }

    const std::string current = trim_ascii_copy(get_window_text(g_console_edit));
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

HWND create_console_control(
    HWND parent,
    const char* class_name,
    const char* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id)
{
    HWND control = CreateWindowExA(
        0,
        class_name,
        text,
        style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleA(nullptr),
        nullptr);
    if (control) {
        ensure_console_visual_resources();
        if (g_console_font) {
            SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_console_font), TRUE);
        }
    }
    return control;
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
    const std::string command = trim_ascii_copy(get_window_text(g_console_edit));
    if (command.empty()) {
        set_console_status_text("Enter a command.");
        return;
    }

    reset_tab_completion_state();
    append_console_output_line("> " + command);

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
        if (g_console_edit && IsWindow(g_console_edit)) {
            SetWindowTextA(g_console_edit, "");
            SetFocus(g_console_edit);
        }
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
        if (g_console_edit && IsWindow(g_console_edit)) {
            SetWindowTextA(g_console_edit, "");
            SetFocus(g_console_edit);
        }
        return;
    }

    if (is_help_command_request(command)) {
        if (print_rf2_command_help()) {
            set_console_status_text("Printed RF2 command list.");
        }
        else {
            set_console_status_text("Could not print RF2 command list.");
        }
        if (g_console_edit && IsWindow(g_console_edit)) {
            SetWindowTextA(g_console_edit, "");
            SetFocus(g_console_edit);
        }
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
    if (g_console_edit && IsWindow(g_console_edit)) {
        SetWindowTextA(g_console_edit, "");
        SetFocus(g_console_edit);
    }
}

void hide_console_window(bool restore_game_focus)
{
    if (!g_console_hooked_game_window || !IsWindow(g_console_hooked_game_window)) {
        return;
    }
    reset_tab_completion_state();
    g_console_is_open = false;
    g_console_target_height = 0;
    SetTimer(g_console_hooked_game_window, console_anim_timer_id, console_anim_timer_interval_ms, nullptr);
    if (restore_game_focus && g_console_hooked_game_window && IsWindow(g_console_hooked_game_window)) {
        ShowWindow(g_console_hooked_game_window, SW_RESTORE);
        BringWindowToTop(g_console_hooked_game_window);
        SetForegroundWindow(g_console_hooked_game_window);
        SetActiveWindow(g_console_hooked_game_window);
        SetFocus(g_console_hooked_game_window);
    }
}

void layout_console_window()
{
    if (!g_console_window || !IsWindow(g_console_window) || !g_console_hooked_game_window || !IsWindow(g_console_hooked_game_window)) {
        return;
    }

    RECT parent_client{};
    GetClientRect(g_console_hooked_game_window, &parent_client);
    const int parent_width = std::max<int>(parent_client.right - parent_client.left, 1);
    const int parent_height = std::max<int>(parent_client.bottom - parent_client.top, 1);
    const int max_open_height = std::max<int>(120, (parent_height * 4) / 5);
    if (g_console_is_open) {
        g_console_target_height = std::min<int>(console_open_height_px, max_open_height);
    }
    else {
        g_console_target_height = 0;
    }

    const int height = std::clamp(g_console_current_height, 0, max_open_height);
    POINT top_left{0, 0};
    ClientToScreen(g_console_hooked_game_window, &top_left);
    MoveWindow(g_console_window, top_left.x, top_left.y, parent_width, height, FALSE);

    const int margin = 8;
    const int input_h = 24;
    const int gap = 8;
    const int help_h = 20;
    const int status_h = 18;
    const int min_output_h = 24;
    const int min_needed_h = (margin * 2) + input_h + gap + help_h + gap + status_h + gap + min_output_h;
    const bool show_controls = height >= min_needed_h;

    auto set_visible = [](HWND control, bool show) {
        if (control && IsWindow(control)) {
            ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
        }
    };

    HWND label = GetDlgItem(g_console_window, id_console_label);
    set_visible(label, show_controls);
    set_visible(g_console_edit, show_controls);
    set_visible(g_console_output, show_controls);
    set_visible(g_console_status, show_controls);
    if (!show_controls) {
        return;
    }

    const int input_y = height - margin - input_h;
    const int help_y = input_y - gap - help_h;
    const int status_y = help_y - gap - status_h;
    const int output_top = margin;
    const int output_bottom = std::max<int>(output_top + min_output_h, status_y - gap);
    const int output_h = std::max<int>(min_output_h, output_bottom - output_top);
    const int content_w = std::max<int>(120, parent_width - (margin * 2));

    if (label) {
        MoveWindow(label, margin, help_y, content_w, help_h, TRUE);
    }
    if (g_console_edit && IsWindow(g_console_edit)) {
        MoveWindow(g_console_edit, margin, input_y, content_w, input_h, TRUE);
    }
    if (g_console_output && IsWindow(g_console_output)) {
        MoveWindow(g_console_output, margin, output_top, content_w, output_h, TRUE);
    }
    if (g_console_status && IsWindow(g_console_status)) {
        MoveWindow(g_console_status, margin, status_y, content_w, status_h, TRUE);
    }

    // Keep painter order deterministic: output at back, status/help in middle, input on top.
    if (g_console_output && IsWindow(g_console_output)) {
        SetWindowPos(g_console_output, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    if (g_console_status && IsWindow(g_console_status)) {
        SetWindowPos(g_console_status, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    if (label) {
        SetWindowPos(label, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    if (g_console_edit && IsWindow(g_console_edit)) {
        SetWindowPos(g_console_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // Keep paint async to avoid hitching the game loop while the console animates.
    InvalidateRect(g_console_window, nullptr, FALSE);
}

void step_console_animation()
{
    if (!g_console_hooked_game_window || !IsWindow(g_console_hooked_game_window)) {
        return;
    }

    if (g_console_current_height < g_console_target_height) {
        g_console_current_height = std::min(g_console_current_height + console_anim_step_px, g_console_target_height);
    }
    else if (g_console_current_height > g_console_target_height) {
        g_console_current_height = std::max(g_console_current_height - console_anim_step_px, g_console_target_height);
    }

    if (g_console_current_height <= 0) {
        g_console_current_height = 0;
        if (g_console_window && IsWindow(g_console_window)) {
            ShowWindow(g_console_window, SW_HIDE);
        }
    }
    else if (g_console_window && IsWindow(g_console_window)) {
        ShowWindow(g_console_window, SW_SHOWNA);
    }

    layout_console_window();

    if (g_console_current_height == g_console_target_height) {
        KillTimer(g_console_hooked_game_window, console_anim_timer_id);
        if (g_console_is_open && g_console_edit && IsWindow(g_console_edit)) {
            SetForegroundWindow(g_console_window);
            SetActiveWindow(g_console_window);
            SetFocus(g_console_edit);
        }
    }
}

bool ensure_console_window(HWND owner)
{
    if (g_console_window && IsWindow(g_console_window)) {
        return true;
    }

    WNDCLASSA window_class{};
    window_class.lpfnWndProc = console_window_proc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = rf2patch_console_class_name;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ensure_console_visual_resources();
    window_class.hbrBackground = g_console_bg_brush ? g_console_bg_brush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        xlog::warn("Failed to register SOPOT console window class");
        return false;
    }

    owner = resolve_target_window(owner);
    if (!owner || !IsWindow(owner)) {
        return false;
    }
    g_console_hooked_game_window = owner;

    RECT owner_client{};
    GetClientRect(owner, &owner_client);
    POINT owner_top_left{0, 0};
    ClientToScreen(owner, &owner_top_left);

    g_console_window = CreateWindowExA(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_CONTROLPARENT,
        rf2patch_console_class_name,
        "SOPOT Developer Console",
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        owner_top_left.x,
        owner_top_left.y,
        std::max<LONG>(owner_client.right - owner_client.left, 1),
        1,
        nullptr,
        nullptr,
        GetModuleHandleA(nullptr),
        nullptr);
    if (!g_console_window) {
        xlog::warn("Failed to create SOPOT developer console window");
        return false;
    }

    apply_console_window_skin(g_console_window);
    SetWindowPos(g_console_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(g_console_window, SW_HIDE);
    return true;
}

void toggle_console_window(HWND owner)
{
    if (!ensure_console_window(owner)) {
        return;
    }

    owner = resolve_target_window(owner);
    if (!owner || !IsWindow(owner)) {
        return;
    }
    g_console_hooked_game_window = owner;
    g_console_is_open = !g_console_is_open;
    if (g_console_is_open) {
        set_console_status_text("Ready. Enter RF2 command and press Enter or Run.");
    }
    layout_console_window();
    SetTimer(g_console_hooked_game_window, console_anim_timer_id, console_anim_timer_interval_ms, nullptr);
    step_console_animation();
}

LRESULT CALLBACK console_edit_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    if (msg == WM_KEYDOWN) {
        if (w_param == VK_OEM_3) {
            return 0;
        }
        if (w_param == VK_PRIOR) {
            scroll_console_output_by(SB_PAGEUP);
            return 0;
        }
        if (w_param == VK_NEXT) {
            scroll_console_output_by(SB_PAGEDOWN);
            return 0;
        }
        if (w_param == VK_HOME) {
            scroll_console_output_to_top();
            return 0;
        }
        if (w_param == VK_END) {
            scroll_console_output_to_bottom();
            return 0;
        }
        if (w_param == VK_TAB) {
            tab_complete_console_input();
            return 0;
        }
        if (w_param == VK_RETURN) {
            run_console_command_from_ui();
            return 0;
        }
        if (w_param == VK_ESCAPE) {
            reset_tab_completion_state();
            hide_console_window(true);
            return 0;
        }
        reset_tab_completion_state();
    }
    if (msg == WM_KEYUP && w_param == VK_OEM_3) {
        reset_tab_completion_state();
        hide_console_window(true);
        return 0;
    }
    if ((msg == WM_CHAR || msg == WM_SYSCHAR) && (w_param == '`' || w_param == '~')) {
        return 0;
    }

    if (g_original_console_edit_proc) {
        return CallWindowProcA(g_original_console_edit_proc, hwnd, msg, w_param, l_param);
    }
    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

LRESULT CALLBACK console_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg) {
    case WM_CREATE: {
        create_console_control(
            hwnd,
            "STATIC",
            "Input: Enter execute, Tab complete, . <text> search, fov/maxfps/r_showfps/directinput/aimslow/enemycrosshair, ~ toggle",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
            10,
            10,
            220,
            18,
            id_console_label);
        g_console_edit = create_console_control(
            hwnd,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            10,
            30,
            734,
            24,
            id_console_edit);
        g_console_output = create_console_control(
            hwnd,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            10,
            62,
            734,
            286,
            id_console_output);
        g_console_status = create_console_control(
            hwnd,
            "STATIC",
            "Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
            10,
            354,
            734,
            18,
            id_console_status);

        if (g_console_edit) {
            auto prev_proc = SetWindowLongPtrA(
                g_console_edit,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(console_edit_proc));
            g_original_console_edit_proc = reinterpret_cast<WndProcFn>(prev_proc);
        }
        refresh_console_output_view();
        return 0;
    }

    case WM_SIZE:
        layout_console_window();
        return 0;

    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        ensure_console_visual_resources();
        FillRect(
            reinterpret_cast<HDC>(w_param),
            &rc,
            g_console_bg_brush ? g_console_bg_brush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }

    case WM_SETFOCUS:
        if (g_console_edit && IsWindow(g_console_edit)) {
            SetFocus(g_console_edit);
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        HWND control = reinterpret_cast<HWND>(l_param);
        ensure_console_visual_resources();
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, RGB(0, 0, 0));
        if (control == g_console_status) {
            SetTextColor(dc, console_status_color);
        }
        else {
            SetTextColor(dc, console_text_color);
        }
        return reinterpret_cast<INT_PTR>(g_console_bg_brush ? g_console_bg_brush : GetStockObject(BLACK_BRUSH));
    }

    case WM_COMMAND: {
        return 0;
    }

    case WM_CLOSE:
        hide_console_window(true);
        return 0;

    case WM_DESTROY:
        g_console_window = nullptr;
        g_console_edit = nullptr;
        g_console_output = nullptr;
        g_console_status = nullptr;
        g_original_console_edit_proc = nullptr;
        g_console_current_height = 0;
        g_console_target_height = 0;
        g_console_is_open = false;
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

LRESULT CALLBACK game_window_proc_hook(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (w_param == VK_OEM_3) {
            return 0;
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (w_param == VK_OEM_3) {
            toggle_console_window(hwnd);
            return 0;
        }
        break;

    case WM_CHAR:
    case WM_SYSCHAR:
        if (w_param == '`' || w_param == '~') {
            return 0;
        }
        break;

    case WM_TIMER:
        if (w_param == console_anim_timer_id) {
            step_console_animation();
            return 0;
        }
        break;

    case WM_SIZE:
        layout_console_window();
        break;

    case WM_NCDESTROY:
        KillTimer(hwnd, console_anim_timer_id);
        if (g_console_window && IsWindow(g_console_window)) {
            DestroyWindow(g_console_window);
        }
        g_console_window = nullptr;
        g_console_edit = nullptr;
        g_console_output = nullptr;
        g_console_status = nullptr;
        g_original_console_edit_proc = nullptr;
        g_console_hooked_game_window = nullptr;
        g_console_current_height = 0;
        g_console_target_height = 0;
        g_console_is_open = false;
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
    layout_console_window();
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
