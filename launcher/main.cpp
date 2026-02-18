#include <launcher_common/PatchedAppLauncher.h>
#include <common/version/version.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <optional>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace
{

constexpr int id_about_button = 1002;
constexpr int id_settings_button = 1003;
constexpr int id_launch_button = 1004;
constexpr int id_exit_button = 1005;
constexpr int id_main_path_summary = 1006;

constexpr int id_settings_mode_combo = 2001;
constexpr int id_settings_fast_start = 2002;
constexpr int id_settings_resolution_combo = 2003;
constexpr int id_settings_auto_close = 2004;
constexpr int id_settings_path_edit = 2005;
constexpr int id_settings_browse_button = 2006;
constexpr int id_settings_vsync = 2007;
constexpr int id_settings_direct_input = 2008;
constexpr int id_settings_aim_slowdown = 2009;
constexpr int id_settings_crosshair_enemy = 2010;

constexpr int id_about_open_changelog = 3001;
constexpr int id_about_open_licensing = 3002;
constexpr int id_about_open_readme = 3003;

constexpr const char* main_window_class_name = "SopotLauncherMainWindow";
constexpr const char* settings_window_class_name = "SopotLauncherSettingsWindow";
constexpr const char* about_window_class_name = "SopotLauncherAboutWindow";
constexpr const char* expected_rf2_sha1 = "5af980c1f2d2588296d40881eb509005b6b3bac9";

enum class LauncherWindowMode
{
    fullscreen,
    windowed,
    borderless,
};

struct ResolutionOption
{
    unsigned width;
    unsigned height;
    const char* label;
};

constexpr ResolutionOption k_resolution_options[] = {
    {640, 480, "640x480"},
    {720, 480, "720x480"},
    {720, 576, "720x576"},
    {800, 600, "800x600"},
    {1176, 664, "1176x664"},
    {1024, 768, "1024x768"},
    {1280, 720, "1280x720"},
    {1280, 768, "1280x768"},
    {1152, 864, "1152x864"},
    {1280, 800, "1280x800"},
    {1360, 768, "1360x768"},
    {1366, 768, "1366x768"},
    {1280, 960, "1280x960"},
    {1280, 1024, "1280x1024"},
    {1600, 900, "1600x900"},
    {1440, 1080, "1440x1080"},
    {1600, 1024, "1600x1024"},
    {1768, 992, "1768x992"},
    {1680, 1050, "1680x1050"},
    {1920, 1080, "1920x1080"},
};

struct PatchSettings
{
    std::string game_exe_path;
    LauncherWindowMode window_mode = LauncherWindowMode::windowed;
    unsigned resolution_width = 1024;
    unsigned resolution_height = 768;
    float fov = 0.0f;
    bool fast_start = true;
    bool vsync = false;
    bool direct_input_mouse = true;
    bool aim_slowdown_on_target = true;
    bool crosshair_enemy_indicator = true;
    bool auto_close_launcher = true;
};

struct LaunchArgs
{
    std::optional<std::string> exe_path;
    std::vector<std::string> passthrough_args;
};

struct SettingsDialogState
{
    PatchSettings* settings = nullptr;
    PatchSettings draft{};
    HWND mode_combo = nullptr;
    HWND path_edit = nullptr;
    HWND resolution_combo = nullptr;
    HWND fast_start_checkbox = nullptr;
    HWND vsync_checkbox = nullptr;
    HWND direct_input_checkbox = nullptr;
    HWND aim_slowdown_checkbox = nullptr;
    HWND crosshair_enemy_checkbox = nullptr;
    HWND auto_close_checkbox = nullptr;
    bool accepted = false;
};

struct MainWindowState
{
    LaunchArgs launch_args;
    PatchSettings settings;
    std::string settings_path;
    std::string initial_game_path;
    HWND path_summary = nullptr;
};

struct AboutDialogState
{
    HWND owner = nullptr;
};

std::string to_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_copy(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool parse_bool_text(const std::string& value)
{
    const std::string lc = to_lower_copy(trim_copy(value));
    return lc == "1" || lc == "true" || lc == "yes" || lc == "on";
}

bool parse_resolution_text(const std::string& value, unsigned& width, unsigned& height)
{
    const std::string text = to_lower_copy(trim_copy(value));
    const size_t sep_pos = text.find('x');
    if (sep_pos == std::string::npos) {
        return false;
    }

    const std::string w_text = trim_copy(text.substr(0, sep_pos));
    const std::string h_text = trim_copy(text.substr(sep_pos + 1));
    if (w_text.empty() || h_text.empty()) {
        return false;
    }

    try {
        unsigned parsed_w = static_cast<unsigned>(std::stoul(w_text));
        unsigned parsed_h = static_cast<unsigned>(std::stoul(h_text));
        if (parsed_w < 320 || parsed_h < 200) {
            return false;
        }
        width = parsed_w;
        height = parsed_h;
        return true;
    }
    catch (...) {
        return false;
    }
}

bool parse_fov_text(const std::string& value, float& fov)
{
    const std::string text = trim_copy(value);
    if (text.empty()) {
        return false;
    }
    try {
        const float parsed = std::stof(text);
        if (parsed < 0.0f) {
            return false;
        }
        fov = parsed;
        return true;
    }
    catch (...) {
        return false;
    }
}

int find_resolution_index(unsigned width, unsigned height)
{
    for (int i = 0; i < static_cast<int>(std::size(k_resolution_options)); ++i) {
        if (k_resolution_options[i].width == width && k_resolution_options[i].height == height) {
            return i;
        }
    }
    for (int i = 0; i < static_cast<int>(std::size(k_resolution_options)); ++i) {
        if (k_resolution_options[i].width == 1024 && k_resolution_options[i].height == 768) {
            return i;
        }
    }
    return 0;
}

std::string format_resolution(unsigned width, unsigned height)
{
    return std::to_string(width) + "x" + std::to_string(height);
}

const char* window_mode_to_ini_value(LauncherWindowMode mode)
{
    switch (mode) {
    case LauncherWindowMode::fullscreen:
        return "fullscreen";
    case LauncherWindowMode::borderless:
        return "borderless";
    case LauncherWindowMode::windowed:
    default:
        return "windowed";
    }
}

LauncherWindowMode window_mode_from_ini_value(const std::string& value)
{
    const std::string mode_lc = to_lower_copy(trim_copy(value));
    if (mode_lc == "fullscreen") {
        return LauncherWindowMode::fullscreen;
    }
    if (mode_lc == "borderless") {
        return LauncherWindowMode::borderless;
    }
    return LauncherWindowMode::windowed;
}

std::string get_module_directory()
{
    char module_path[MAX_PATH] = {};
    const DWORD path_len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (path_len == 0 || path_len >= MAX_PATH) {
        return ".";
    }

    std::string full_path{module_path};
    const size_t pos = full_path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return ".";
    }
    return full_path.substr(0, pos);
}

std::string get_settings_file_path()
{
    return get_module_directory() + "\\sopot_settings.ini";
}

bool is_existing_regular_file(const std::string& path)
{
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::optional<std::string> compute_file_hash_hex(const std::string& path, ALG_ID alg_id)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;

    if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return std::nullopt;
    }

    if (!CryptCreateHash(provider, alg_id, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return std::nullopt;
    }

    std::array<char, 16 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer.data()), static_cast<DWORD>(bytes_read), 0)) {
                CryptDestroyHash(hash);
                CryptReleaseContext(provider, 0);
                return std::nullopt;
            }
        }
    }
    if (file.bad()) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return std::nullopt;
    }

    DWORD hash_size = 0;
    DWORD hash_size_len = sizeof(hash_size);
    if (!CryptGetHashParam(hash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hash_size), &hash_size_len, 0)
        || hash_size == 0) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return std::nullopt;
    }

    std::vector<BYTE> hash_bytes(hash_size);
    DWORD hash_len = hash_size;
    if (!CryptGetHashParam(hash, HP_HASHVAL, hash_bytes.data(), &hash_len, 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return std::nullopt;
    }

    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string hex;
    hex.resize(static_cast<size_t>(hash_len) * 2);
    for (DWORD i = 0; i < hash_len; ++i) {
        const BYTE value = hash_bytes[i];
        hex[static_cast<size_t>(i) * 2] = hex_digits[(value >> 4) & 0x0F];
        hex[static_cast<size_t>(i) * 2 + 1] = hex_digits[value & 0x0F];
    }
    return hex;
}

bool validate_rf2_exe_hashes(const std::string& exe_path, std::string& error_message)
{
    if (!is_existing_regular_file(exe_path)) {
        error_message = "Could not find selected rf2.exe:\n" + exe_path;
        return false;
    }

    const auto sha1 = compute_file_hash_hex(exe_path, CALG_SHA1);
    if (!sha1) {
        error_message = "Failed to compute SHA-1 for rf2.exe.\nPath:\n" + exe_path;
        return false;
    }

    if (*sha1 == expected_rf2_sha1) {
        return true;
    }

    error_message =
        "The selected rf2.exe does not match the supported SHA-1 hash. You need to use the RF2 1.01 NA Retail rf2.exe\n\n"
        "Path:\n" + exe_path + "\n\n"
        "Expected SHA-1: " + std::string(expected_rf2_sha1) + "\n"
        "Actual SHA-1:   " + *sha1;
    return false;
}

PatchSettings load_patch_settings(const std::string& settings_path)
{
    PatchSettings settings{};

    char game_exe_path_buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringA(
        "sopot",
        "game_exe_path",
        "",
        game_exe_path_buf,
        static_cast<DWORD>(sizeof(game_exe_path_buf)),
        settings_path.c_str());
    settings.game_exe_path = trim_copy(game_exe_path_buf);

    char window_mode_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "window_mode",
        "windowed",
        window_mode_buf,
        static_cast<DWORD>(sizeof(window_mode_buf)),
        settings_path.c_str());
    settings.window_mode = window_mode_from_ini_value(window_mode_buf);

    char fast_start_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "fast_start",
        "1",
        fast_start_buf,
        static_cast<DWORD>(sizeof(fast_start_buf)),
        settings_path.c_str());
    settings.fast_start = parse_bool_text(fast_start_buf);

    char vsync_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "vsync",
        "0",
        vsync_buf,
        static_cast<DWORD>(sizeof(vsync_buf)),
        settings_path.c_str());
    settings.vsync = parse_bool_text(vsync_buf);

    char direct_input_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "direct_input_mouse",
        "1",
        direct_input_buf,
        static_cast<DWORD>(sizeof(direct_input_buf)),
        settings_path.c_str());
    settings.direct_input_mouse = parse_bool_text(direct_input_buf);

    char aim_slowdown_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "aim_slowdown_on_target",
        "1",
        aim_slowdown_buf,
        static_cast<DWORD>(sizeof(aim_slowdown_buf)),
        settings_path.c_str());
    settings.aim_slowdown_on_target = parse_bool_text(aim_slowdown_buf);

    char crosshair_enemy_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "crosshair_enemy_indicator",
        "1",
        crosshair_enemy_buf,
        static_cast<DWORD>(sizeof(crosshair_enemy_buf)),
        settings_path.c_str());
    settings.crosshair_enemy_indicator = parse_bool_text(crosshair_enemy_buf);

    char auto_close_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "auto_close_launcher",
        "1",
        auto_close_buf,
        static_cast<DWORD>(sizeof(auto_close_buf)),
        settings_path.c_str());
    settings.auto_close_launcher = parse_bool_text(auto_close_buf);

    char resolution_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "resolution",
        "1024x768",
        resolution_buf,
        static_cast<DWORD>(sizeof(resolution_buf)),
        settings_path.c_str());
    unsigned width = settings.resolution_width;
    unsigned height = settings.resolution_height;
    if (parse_resolution_text(resolution_buf, width, height)) {
    settings.resolution_width = width;
    settings.resolution_height = height;
}

    char fov_buf[64] = {};
    GetPrivateProfileStringA(
        "sopot",
        "fov",
        "0",
        fov_buf,
        static_cast<DWORD>(sizeof(fov_buf)),
        settings_path.c_str());
    float fov_value = settings.fov;
    if (parse_fov_text(fov_buf, fov_value)) {
        settings.fov = fov_value;
    }

    return settings;
}

bool save_patch_settings(const std::string& settings_path, const PatchSettings& settings)
{
    const BOOL wrote_exe_path = WritePrivateProfileStringA(
        "sopot",
        "game_exe_path",
        settings.game_exe_path.c_str(),
        settings_path.c_str());
    const BOOL wrote_mode = WritePrivateProfileStringA(
        "sopot",
        "window_mode",
        window_mode_to_ini_value(settings.window_mode),
        settings_path.c_str());
    const std::string resolution_value = format_resolution(settings.resolution_width, settings.resolution_height);
    const BOOL wrote_resolution = WritePrivateProfileStringA(
        "sopot",
        "resolution",
        resolution_value.c_str(),
        settings_path.c_str());
    const BOOL wrote_fast_start = WritePrivateProfileStringA(
        "sopot",
        "fast_start",
        settings.fast_start ? "1" : "0",
        settings_path.c_str());
    const BOOL wrote_vsync = WritePrivateProfileStringA(
        "sopot",
        "vsync",
        settings.vsync ? "1" : "0",
        settings_path.c_str());
    const BOOL wrote_direct_input = WritePrivateProfileStringA(
        "sopot",
        "direct_input_mouse",
        settings.direct_input_mouse ? "1" : "0",
        settings_path.c_str());
    const BOOL wrote_aim_slowdown = WritePrivateProfileStringA(
        "sopot",
        "aim_slowdown_on_target",
        settings.aim_slowdown_on_target ? "1" : "0",
        settings_path.c_str());
    const BOOL wrote_crosshair_enemy = WritePrivateProfileStringA(
        "sopot",
        "crosshair_enemy_indicator",
        settings.crosshair_enemy_indicator ? "1" : "0",
        settings_path.c_str());
    const BOOL wrote_auto_close = WritePrivateProfileStringA(
        "sopot",
        "auto_close_launcher",
        settings.auto_close_launcher ? "1" : "0",
        settings_path.c_str());
    char fov_value[64] = {};
    std::snprintf(fov_value, sizeof(fov_value), "%.3f", settings.fov);
    const BOOL wrote_fov = WritePrivateProfileStringA(
        "sopot",
        "fov",
        fov_value,
        settings_path.c_str());

    return wrote_exe_path != FALSE
        && wrote_mode != FALSE
        && wrote_resolution != FALSE
        && wrote_fast_start != FALSE
        && wrote_vsync != FALSE
        && wrote_direct_input != FALSE
        && wrote_aim_slowdown != FALSE
        && wrote_crosshair_enemy != FALSE
        && wrote_auto_close != FALSE
        && wrote_fov != FALSE;
}

LaunchArgs parse_launch_args(int argc, char** argv)
{
    LaunchArgs result;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--rf2-exe" || arg == "--exe") {
            if (i + 1 < argc) {
                result.exe_path = argv[++i];
            }
            continue;
        }

        const std::string exe_prefix = "--rf2-exe=";
        if (arg.rfind(exe_prefix, 0) == 0) {
            result.exe_path = arg.substr(exe_prefix.size());
            continue;
        }

        result.passthrough_args.push_back(arg);
    }
    return result;
}

std::string get_edit_text(HWND edit_control)
{
    const int text_len = GetWindowTextLengthA(edit_control);
    if (text_len <= 0) {
        return {};
    }

    std::string buffer(static_cast<size_t>(text_len) + 1, '\0');
    GetWindowTextA(edit_control, buffer.data(), text_len + 1);
    buffer.resize(static_cast<size_t>(text_len));
    return buffer;
}

void update_main_path_summary(MainWindowState* state)
{
    if (!state || !state->path_summary || !IsWindow(state->path_summary)) {
        return;
    }
    std::string text = "Configured rf2.exe: ";
    if (state->settings.game_exe_path.empty()) {
        text += "<not set>";
    }
    else {
        text += state->settings.game_exe_path;
    }
    SetWindowTextA(state->path_summary, text.c_str());
}

std::optional<std::string> browse_for_rf2_exe(HWND owner, const std::string& current_path)
{
    char file_path[MAX_PATH] = {};
    if (!current_path.empty()) {
        strncpy_s(file_path, current_path.c_str(), _TRUNCATE);
    }

    char filter[] =
        "Red Faction II executable (rf2.exe)\0rf2.exe\0"
        "Executable files (*.exe)\0*.exe\0"
        "All files (*.*)\0*.*\0";

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = "Select Red Faction II executable";

    if (!GetOpenFileNameA(&ofn)) {
        return std::nullopt;
    }
    return std::string{file_path};
}

HWND create_control(HWND parent, const char* class_name, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND control = CreateWindowExA(
        0,
        class_name,
        text,
        style,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleA(nullptr),
        nullptr);
    if (control) {
        SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    return control;
}

bool open_target_with_shell(HWND owner, const std::string& target)
{
    HINSTANCE result = ShellExecuteA(owner, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool open_module_document(HWND owner, const char* file_name, const char* friendly_name)
{
    const std::string full_path = get_module_directory() + "\\" + file_name;
    const DWORD attributes = GetFileAttributesA(full_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        std::string message = "Could not find ";
        message += friendly_name;
        message += ":\n";
        message += full_path;
        MessageBoxA(owner, message.c_str(), "RF2 Community Patch (SOPOT)", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!open_target_with_shell(owner, full_path)) {
        std::string message = "Failed to open ";
        message += friendly_name;
        message += ":\n";
        message += full_path;
        MessageBoxA(owner, message.c_str(), "RF2 Community Patch (SOPOT)", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

LRESULT CALLBACK about_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* create_struct = reinterpret_cast<CREATESTRUCTA*>(l_param);
        state = reinterpret_cast<AboutDialogState*>(create_struct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        const std::string version_text = std::string("Version: ") + VERSION_STR;

        create_control(hwnd, "STATIC", "RF2 Community Patch (SOPOT)", WS_CHILD | WS_VISIBLE, 16, 10, 320, 22, 0);
        create_control(hwnd, "STATIC", version_text.c_str(), WS_CHILD | WS_VISIBLE, 16, 32, 320, 18, 0);
        create_control(
            hwnd,
            "STATIC",
            "RF2 Community Patch (SOPOT) is a compatibility and enhancement patch for Red Faction II.",
            WS_CHILD | WS_VISIBLE,
            16,
            56,
            388,
            18,
            0);
        create_control(
            hwnd,
            "STATIC",
            "This software uses open-source components. See licensing info for details.",
            WS_CHILD | WS_VISIBLE,
            16,
            76,
            388,
            18,
            0);

        create_control(
            hwnd,
            "BUTTON",
            "Open changelog",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            16,
            108,
            122,
            26,
            id_about_open_changelog);
        create_control(
            hwnd,
            "BUTTON",
            "Licensing info",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            144,
            108,
            122,
            26,
            id_about_open_licensing);
        create_control(
            hwnd,
            "BUTTON",
            "Project README",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            272,
            108,
            122,
            26,
            id_about_open_readme);

        create_control(
            hwnd,
            "BUTTON",
            "OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            170,
            146,
            74,
            26,
            IDOK);
        return 0;
    }

    case WM_COMMAND: {
        const int control_id = LOWORD(w_param);
        if (control_id == id_about_open_changelog) {
            open_module_document(hwnd, "changelog.md", "changelog.md");
            return 0;
        }
        if (control_id == id_about_open_licensing) {
            open_module_document(hwnd, "licensing-info.txt", "licensing-info.txt");
            return 0;
        }
        if (control_id == id_about_open_readme) {
            open_module_document(hwnd, "README.md", "README.md");
            return 0;
        }
        if (control_id == IDOK || control_id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

void show_about_dialog(HWND owner)
{
    WNDCLASSA window_class = {};
    window_class.lpfnWndProc = about_window_proc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = about_window_class_name;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&window_class);

    AboutDialogState state{};
    state.owner = owner;

    HWND dialog = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        about_window_class_name,
        "About RF2 Community Patch (SOPOT)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        428,
        214,
        owner,
        nullptr,
        GetModuleHandleA(nullptr),
        &state);
    if (!dialog) {
        MessageBoxA(owner, "Failed to open About dialog.", "RF2 Community Patch (SOPOT)", MB_OK | MB_ICONERROR);
        return;
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg = {};
    while (IsWindow(dialog) && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);
}

LRESULT CALLBACK settings_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* create_struct = reinterpret_cast<CREATESTRUCTA*>(l_param);
        state = reinterpret_cast<SettingsDialogState*>(create_struct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        create_control(
            hwnd,
            "STATIC",
            "Red Faction II executable:",
            WS_CHILD | WS_VISIBLE,
            14,
            16,
            170,
            20,
            0);
        state->path_edit = create_control(
            hwnd,
            "EDIT",
            state->draft.game_exe_path.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            14,
            36,
            220,
            24,
            id_settings_path_edit);
        create_control(
            hwnd,
            "BUTTON",
            "Browse...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            240,
            36,
            52,
            24,
            id_settings_browse_button);
        create_control(
            hwnd,
            "STATIC",
            "Window mode:",
            WS_CHILD | WS_VISIBLE,
            14,
            70,
            92,
            20,
            0);
        state->mode_combo = create_control(
            hwnd,
            "COMBOBOX",
            "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            112,
            66,
            180,
            120,
            id_settings_mode_combo);
        SendMessageA(state->mode_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Fullscreen"));
        SendMessageA(state->mode_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Windowed"));
        SendMessageA(state->mode_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Borderless"));

        int mode_index = 1;
        if (state->draft.window_mode == LauncherWindowMode::fullscreen) {
            mode_index = 0;
        }
        else if (state->draft.window_mode == LauncherWindowMode::borderless) {
            mode_index = 2;
        }
        SendMessageA(state->mode_combo, CB_SETCURSEL, mode_index, 0);

        create_control(
            hwnd,
            "STATIC",
            "Resolution:",
            WS_CHILD | WS_VISIBLE,
            14,
            106,
            92,
            20,
            0);
        state->resolution_combo = create_control(
            hwnd,
            "COMBOBOX",
            "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            112,
            102,
            180,
            240,
            id_settings_resolution_combo);
        for (const auto& option : k_resolution_options) {
            SendMessageA(state->resolution_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.label));
        }
        SendMessageA(
            state->resolution_combo,
            CB_SETCURSEL,
            find_resolution_index(state->draft.resolution_width, state->draft.resolution_height),
            0);

        state->fast_start_checkbox = create_control(
            hwnd,
            "BUTTON",
            "Fast start (skip startup BIK logos)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            138,
            278,
            24,
            id_settings_fast_start);
        SendMessageA(
            state->fast_start_checkbox,
            BM_SETCHECK,
            state->draft.fast_start ? BST_CHECKED : BST_UNCHECKED,
            0);

        state->auto_close_checkbox = create_control(
            hwnd,
            "BUTTON",
            "Auto close launcher after launch",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            166,
            278,
            24,
            id_settings_auto_close);
        SendMessageA(
            state->auto_close_checkbox,
            BM_SETCHECK,
            state->draft.auto_close_launcher ? BST_CHECKED : BST_UNCHECKED,
            0);

        state->vsync_checkbox = create_control(
            hwnd,
            "BUTTON",
            "VSync",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            194,
            278,
            24,
            id_settings_vsync);
        SendMessageA(
            state->vsync_checkbox,
            BM_SETCHECK,
            state->draft.vsync ? BST_CHECKED : BST_UNCHECKED,
            0);

        state->direct_input_checkbox = create_control(
            hwnd,
            "BUTTON",
            "DirectInput mouse (menus + gameplay)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            222,
            278,
            24,
            id_settings_direct_input);
        SendMessageA(
            state->direct_input_checkbox,
            BM_SETCHECK,
            state->draft.direct_input_mouse ? BST_CHECKED : BST_UNCHECKED,
            0);

        state->aim_slowdown_checkbox = create_control(
            hwnd,
            "BUTTON",
            "Aim slowdown when targeting enemy",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            250,
            278,
            24,
            id_settings_aim_slowdown);
        SendMessageA(
            state->aim_slowdown_checkbox,
            BM_SETCHECK,
            state->draft.aim_slowdown_on_target ? BST_CHECKED : BST_UNCHECKED,
            0);

        state->crosshair_enemy_checkbox = create_control(
            hwnd,
            "BUTTON",
            "Crosshair changes on enemy target",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14,
            278,
            278,
            24,
            id_settings_crosshair_enemy);
        SendMessageA(
            state->crosshair_enemy_checkbox,
            BM_SETCHECK,
            state->draft.crosshair_enemy_indicator ? BST_CHECKED : BST_UNCHECKED,
            0);

        create_control(
            hwnd,
            "BUTTON",
            "Save",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            130,
            308,
            78,
            26,
            IDOK);
        create_control(
            hwnd,
            "BUTTON",
            "Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            214,
            308,
            78,
            26,
            IDCANCEL);
        return 0;
    }

    case WM_COMMAND: {
        const int control_id = LOWORD(w_param);
        if (control_id == id_settings_browse_button && state) {
            auto selected_path = browse_for_rf2_exe(hwnd, get_edit_text(state->path_edit));
            if (selected_path) {
                SetWindowTextA(state->path_edit, selected_path->c_str());
            }
            return 0;
        }
        if (control_id == IDOK && state && state->settings) {
            state->draft.game_exe_path = trim_copy(get_edit_text(state->path_edit));
            int mode_index = static_cast<int>(SendMessageA(state->mode_combo, CB_GETCURSEL, 0, 0));
            if (mode_index == 0) {
                state->draft.window_mode = LauncherWindowMode::fullscreen;
            }
            else if (mode_index == 2) {
                state->draft.window_mode = LauncherWindowMode::borderless;
            }
            else {
                state->draft.window_mode = LauncherWindowMode::windowed;
            }
            int resolution_index = static_cast<int>(SendMessageA(state->resolution_combo, CB_GETCURSEL, 0, 0));
            if (resolution_index < 0 || resolution_index >= static_cast<int>(std::size(k_resolution_options))) {
                resolution_index = find_resolution_index(1024, 768);
            }
            state->draft.resolution_width = k_resolution_options[resolution_index].width;
            state->draft.resolution_height = k_resolution_options[resolution_index].height;
            state->draft.fast_start = SendMessageA(state->fast_start_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->draft.vsync = SendMessageA(state->vsync_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->draft.direct_input_mouse =
                SendMessageA(state->direct_input_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->draft.aim_slowdown_on_target =
                SendMessageA(state->aim_slowdown_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->draft.crosshair_enemy_indicator =
                SendMessageA(state->crosshair_enemy_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->draft.auto_close_launcher =
                SendMessageA(state->auto_close_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

            *state->settings = state->draft;
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (control_id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

bool show_settings_dialog(HWND owner, PatchSettings& settings)
{
    WNDCLASSA window_class = {};
    window_class.lpfnWndProc = settings_window_proc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = settings_window_class_name;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&window_class);

    SettingsDialogState state{};
    state.settings = &settings;
    state.draft = settings;

    HWND dialog = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        settings_window_class_name,
        "RF2 Community Patch (SOPOT) Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        400,
        owner,
        nullptr,
        GetModuleHandleA(nullptr),
        &state);
    if (!dialog) {
        return false;
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg = {};
    while (IsWindow(dialog) && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);
    return state.accepted;
}

bool launch_game(const LaunchArgs& launch_args, const std::string& game_path, std::string& error_message)
{
    try {
        GameLauncher launcher;
        launcher.set_args(launch_args.passthrough_args);
        launcher.set_app_exe_path(game_path);
        launcher.launch();
        return true;
    }
    catch (const PrivilegeElevationRequiredException&) {
        error_message =
            "Launch failed due to an elevation requirement on rf2.exe.\n"
            "Disable any \"Run this program as administrator\" compatibility option for rf2.exe and try again.";
    }
    catch (const FileNotFoundException& e) {
        error_message = "Could not find game executable:\n" + e.get_file_name();
    }
    catch (const std::exception& e) {
        error_message = e.what();
    }
    return false;
}

LRESULT CALLBACK main_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<MainWindowState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* create_struct = reinterpret_cast<CREATESTRUCTA*>(l_param);
        state = reinterpret_cast<MainWindowState*>(create_struct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        create_control(
            hwnd,
            "STATIC",
            "RF2 Community Patch (SOPOT) Launcher",
            WS_CHILD | WS_VISIBLE,
            14,
            16,
            220,
            20,
            0);
        state->path_summary = create_control(
            hwnd,
            "STATIC",
            "",
            WS_CHILD | WS_VISIBLE,
            14,
            40,
            592,
            20,
            id_main_path_summary);
        update_main_path_summary(state);
        create_control(
            hwnd,
            "BUTTON",
            "About",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            258,
            84,
            86,
            28,
            id_about_button);
        create_control(
            hwnd,
            "BUTTON",
            "Settings",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            350,
            84,
            86,
            28,
            id_settings_button);
        create_control(
            hwnd,
            "BUTTON",
            "Launch",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            442,
            84,
            86,
            28,
            id_launch_button);
        create_control(
            hwnd,
            "BUTTON",
            "Exit",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            534,
            84,
            86,
            28,
            id_exit_button);
        return 0;
    }

    case WM_COMMAND: {
        const int control_id = LOWORD(w_param);
        if (!state) {
            return 0;
        }

        if (control_id == id_settings_button) {
            PatchSettings new_settings = state->settings;
            if (show_settings_dialog(hwnd, new_settings)) {
                state->settings = new_settings;
                update_main_path_summary(state);
                if (!save_patch_settings(state->settings_path, state->settings)) {
                    MessageBoxA(
                        hwnd,
                        "Failed to save sopot_settings.ini.",
                        "RF2 Community Patch (SOPOT)",
                        MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }

        if (control_id == id_about_button) {
            show_about_dialog(hwnd);
            return 0;
        }

        if (control_id == id_launch_button) {
            const std::string game_path = trim_copy(state->settings.game_exe_path);
            if (game_path.empty()) {
                MessageBoxA(
                    hwnd,
                    "Please set your rf2.exe path in Settings first.",
                    "RF2 Community Patch (SOPOT)",
                    MB_OK | MB_ICONERROR);
                return 0;
            }

            std::string hash_validation_error;
            if (!validate_rf2_exe_hashes(game_path, hash_validation_error)) {
                MessageBoxA(
                    hwnd,
                    hash_validation_error.c_str(),
                    "RF2 Community Patch (SOPOT)",
                    MB_OK | MB_ICONERROR);
                return 0;
            }

            state->settings.game_exe_path = game_path;
            if (!save_patch_settings(state->settings_path, state->settings)) {
                MessageBoxA(
                    hwnd,
                    "Failed to save sopot_settings.ini.",
                    "RF2 Community Patch (SOPOT)",
                    MB_OK | MB_ICONERROR);
                return 0;
            }

            std::string launch_error;
            if (launch_game(state->launch_args, game_path, launch_error)) {
                if (state->settings.auto_close_launcher) {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            MessageBoxA(
                hwnd,
                launch_error.c_str(),
                "RF2 Community Patch (SOPOT)",
                MB_OK | MB_ICONERROR);
            return 0;
        }

        if (control_id == id_exit_button) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

std::string get_initial_game_path(const LaunchArgs& args, const PatchSettings& settings)
{
    if (args.exe_path && !args.exe_path->empty()) {
        return *args.exe_path;
    }
    if (!settings.game_exe_path.empty()) {
        return settings.game_exe_path;
    }

    try {
        GameLauncher launcher;
        return launcher.get_default_app_path();
    }
    catch (...) {
        return {};
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd)
{
    LaunchArgs launch_args = parse_launch_args(__argc, __argv);
    const std::string settings_path = get_settings_file_path();
    PatchSettings settings = load_patch_settings(settings_path);
    if (launch_args.exe_path && !launch_args.exe_path->empty()) {
        settings.game_exe_path = *launch_args.exe_path;
    }
    const std::string initial_game_path = get_initial_game_path(launch_args, settings);
    if (settings.game_exe_path.empty() && !initial_game_path.empty()) {
        settings.game_exe_path = initial_game_path;
    }
    save_patch_settings(settings_path, settings);

    WNDCLASSA window_class = {};
    window_class.lpfnWndProc = main_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = main_window_class_name;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&window_class);

    auto* state = new MainWindowState{};
    state->launch_args = launch_args;
    state->settings = settings;
    state->settings_path = settings_path;
    state->initial_game_path = initial_game_path;

    HWND window = CreateWindowExA(
        0,
        main_window_class_name,
        "RF2 Community Patch (SOPOT) Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        160,
        nullptr,
        nullptr,
        instance,
        state);
    if (!window) {
        delete state;
        MessageBoxA(
            nullptr,
            "Failed to create launcher window.",
            "RF2 Community Patch (SOPOT)",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, show_cmd == 0 ? SW_SHOWDEFAULT : show_cmd);
    UpdateWindow(window);

    MSG msg = {};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}
