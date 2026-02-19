#include "main.h"
#include "../misc/misc.h"
#include <crash_handler_stub.h>
#include <xlog/FileAppender.h>
#include <xlog/LoggerConfig.h>
#include <xlog/xlog.h>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

static HMODULE g_module = nullptr;

static void init_logging()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    CreateDirectoryA("logs", nullptr);
    auto& logger_config = xlog::LoggerConfig::get();
    if (logger_config.get_appenders().empty()) {
        logger_config.add_appender<xlog::FileAppender>("logs\\SOPOT.log", false, true);
    }

    xlog::info("SOPOT logging initialized");
    xlog::info("Command line: {}", GetCommandLineA());
    initialized = true;
}

static std::string trim_copy(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static std::string to_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static bool parse_bool_value(const std::string& value)
{
    const std::string lc = to_lower_copy(trim_copy(value));
    return lc == "1" || lc == "true" || lc == "yes" || lc == "on";
}

static bool parse_fov_value(const std::string& value, float& fov)
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

static bool parse_max_fps_value(const std::string& value, float& max_fps)
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
        max_fps = parsed;
        return true;
    }
    catch (...) {
        return false;
    }
}

static bool parse_resolution_value(const std::string& value, unsigned& width, unsigned& height)
{
    std::string text = to_lower_copy(trim_copy(value));
    size_t sep_pos = text.find('x');
    if (sep_pos == std::string::npos) {
        return false;
    }

    std::string w_text = trim_copy(text.substr(0, sep_pos));
    std::string h_text = trim_copy(text.substr(sep_pos + 1));
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

static std::string get_module_directory(HMODULE module)
{
    char module_path[MAX_PATH] = {};
    DWORD path_len = GetModuleFileNameA(module, module_path, MAX_PATH);
    if (path_len == 0 || path_len >= MAX_PATH) {
        return ".";
    }

    std::string full_path{module_path};
    size_t pos = full_path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return ".";
    }
    return full_path.substr(0, pos);
}

static Rf2PatchSettings load_patch_settings()
{
    Rf2PatchSettings settings{};
    const std::string settings_path = get_module_directory(g_module) + "\\sopot_settings.ini";
    settings.settings_file_path = settings_path;
    std::ifstream file(settings_path);
    if (!file.is_open()) {
        xlog::warn("Settings file not found: {} (using defaults)", settings_path);
        return settings;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == ';' || line[0] == '#'
            || line[0] == '[') {
            continue;
        }

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = to_lower_copy(trim_copy(line.substr(0, eq_pos)));
        std::string value = trim_copy(line.substr(eq_pos + 1));
        if (key == "window_mode") {
            std::string mode_lc = to_lower_copy(value);
            if (mode_lc == "fullscreen") {
                settings.window_mode = Rf2PatchWindowMode::fullscreen;
            }
            else if (mode_lc == "borderless") {
                settings.window_mode = Rf2PatchWindowMode::borderless;
            }
            else {
                settings.window_mode = Rf2PatchWindowMode::windowed;
            }
        }
        else if (key == "fast_start") {
            settings.fast_start = parse_bool_value(value);
        }
        else if (key == "resolution") {
            unsigned w = settings.window_width;
            unsigned h = settings.window_height;
            if (parse_resolution_value(value, w, h)) {
                settings.window_width = w;
                settings.window_height = h;
            }
        }
        else if (key == "fov") {
            float fov_value = settings.fov;
            if (parse_fov_value(value, fov_value)) {
                settings.fov = fov_value;
            }
        }
        else if (key == "max_fps") {
            float max_fps_value = settings.max_fps;
            if (parse_max_fps_value(value, max_fps_value)) {
                settings.max_fps = max_fps_value;
            }
        }
        else if (key == "vsync") {
            settings.vsync = parse_bool_value(value);
        }
        else if (key == "direct_input_mouse") {
            settings.direct_input_mouse = parse_bool_value(value);
        }
        else if (key == "aim_slowdown_on_target") {
            settings.aim_slowdown_on_target = parse_bool_value(value);
        }
        else if (key == "crosshair_enemy_indicator") {
            settings.crosshair_enemy_indicator = parse_bool_value(value);
        }
        else if (key == "r_showfps") {
            settings.r_showfps = parse_bool_value(value);
        }
        else if (key == "experimental_fps_stabilization") {
            settings.experimental_fps_stabilization = parse_bool_value(value);
        }
    }

    const char* mode_name = "windowed";
    if (settings.window_mode == Rf2PatchWindowMode::fullscreen) {
        mode_name = "fullscreen";
    }
    else if (settings.window_mode == Rf2PatchWindowMode::borderless) {
        mode_name = "borderless";
    }

    xlog::info(
        "Loaded settings from {}: window_mode={}, resolution={}x{}, fast_start={}, vsync={}, direct_input_mouse={}, aim_slowdown_on_target={}, crosshair_enemy_indicator={}, r_showfps={}, experimental_fps_stabilization={}, fov={}, max_fps={}",
        settings_path,
        mode_name,
        settings.window_width,
        settings.window_height,
        settings.fast_start ? 1 : 0,
        settings.vsync ? 1 : 0,
        settings.direct_input_mouse ? 1 : 0,
        settings.aim_slowdown_on_target ? 1 : 0,
        settings.crosshair_enemy_indicator ? 1 : 0,
        settings.r_showfps ? 1 : 0,
        settings.experimental_fps_stabilization ? 1 : 0,
        settings.fov,
        settings.max_fps);
    return settings;
}

extern "C" BOOL APIENTRY DllMain(HMODULE h_module, DWORD ul_reason_for_call, LPVOID /*lp_reserved*/)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_module = h_module;
        DisableThreadLibraryCalls(h_module);
        break;
    case DLL_PROCESS_DETACH:
        CrashHandlerStubUninstall();
        break;
    default:
        break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD WINAPI Init(LPVOID /*param*/)
{
    init_logging();

    CrashHandlerConfig crash_config;
    crash_config.this_module_handle = g_module;
    std::strcpy(crash_config.app_name, "SOPOT");
    std::strcpy(crash_config.output_dir, "logs");
    std::strcpy(crash_config.log_file, "SOPOT-crash.log");
    crash_config.add_known_module("rf2.exe");
    crash_config.add_known_module("Sopot.dll");
    CrashHandlerStubInstall(crash_config);
    misc_apply_patches(load_patch_settings());

    xlog::info("SOPOT Init completed");
    return 1;
}

extern "C" void subhook_unk_opcode_handler(uint8_t* opcode)
{
    xlog::error("SubHook unknown opcode 0x{:x} at {}", *opcode, static_cast<void*>(opcode));
}
