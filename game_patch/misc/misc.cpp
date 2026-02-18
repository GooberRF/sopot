#include "misc.h"
#include "../core/console.h"
#include "../core/frame_limiter.h"
#include "../core/high_fps.h"
#include "../player/camera.h"
#include "../rf2/gr/gr.h"
#include "../rf2/os/input.h"
#include "../rf2/os/window.h"
#include "../rf2/player/autoaim.h"
#include "../rf2/player/reticle.h"
#include "../rf2/rf2.h"
#include <patch_common/FunHook.h>
#include <patch_common/AsmOpcodes.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <windows.h>
#include <d3d8.h>
#include <xlog/xlog.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr UINT default_window_width = 1024;
constexpr UINT default_window_height = 768;
constexpr int default_window_x = 40;
constexpr int default_window_y = 40;
constexpr size_t d3d8_create_device_vtbl_index = 15;
constexpr size_t d3d8_device_reset_vtbl_index = 14;
constexpr size_t d3d8_device_present_vtbl_index = 15;

Rf2PatchSettings g_settings{};
bool g_force_window_mode = true;
bool g_force_resolution = true;
bool g_borderless_mode = false;
bool g_fast_start_enabled = true;
bool g_direct_input_mouse_enabled = true;
bool g_aim_slowdown_on_target_enabled = true;
bool g_crosshair_enemy_indicator_enabled = true;
UINT g_forced_window_width = default_window_width;
UINT g_forced_window_height = default_window_height;
int g_forced_window_x = default_window_x;
int g_forced_window_y = default_window_y;
int g_forced_window_mode = static_cast<int>(rf2::gr::WindowMode::windowed);

void configure_window_mode_settings()
{
    g_force_window_mode = g_settings.window_mode != Rf2PatchWindowMode::fullscreen;
    g_force_resolution = true;
    g_borderless_mode = g_settings.window_mode == Rf2PatchWindowMode::borderless;
    g_forced_window_mode = static_cast<int>(rf2::gr::WindowMode::windowed);
    g_forced_window_width = std::max<unsigned>(g_settings.window_width, 320);
    g_forced_window_height = std::max<unsigned>(g_settings.window_height, 200);

    if (!g_force_window_mode) {
        camera_set_resolution(g_forced_window_width, g_forced_window_height);
        xlog::info(
            "Window mode setting: fullscreen (no forced window patch, selected resolution {}x{})",
            g_forced_window_width,
            g_forced_window_height);
        return;
    }

    if (g_borderless_mode) {
        g_forced_window_x = 0;
        g_forced_window_y = 0;
        camera_set_resolution(g_forced_window_width, g_forced_window_height);

        xlog::info(
            "Window mode setting: borderless {}x{} at {},{}",
            g_forced_window_width,
            g_forced_window_height,
            g_forced_window_x,
            g_forced_window_y);
        return;
    }

    g_forced_window_x = default_window_x;
    g_forced_window_y = default_window_y;
    camera_set_resolution(g_forced_window_width, g_forced_window_height);
    xlog::info(
        "Window mode setting: windowed {}x{} at {},{}",
        g_forced_window_width,
        g_forced_window_height,
        g_forced_window_x,
        g_forced_window_y);
}

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

bool patch_vram_check_opcode(uint8_t expected_jcc_opcode)
{
    static constexpr std::array<int, 53> pattern = {
        0xA1, -1, -1, -1, -1,
        0x50,
        0x8B, 0x10,
        0xFF, 0x52, 0x10,
        0x3D, -1, -1, -1, -1,
        -1, -1,
        0xE8, -1, -1, -1, -1,
        0x68, 0x00, 0x20, 0x01, 0x00,
        0x68, -1, -1, -1, -1,
        0x68, -1, -1, -1, -1,
        0x6A, 0x00,
        0xFF, 0x15, -1, -1, -1, -1,
        0x6A, 0x01,
        0xE8, -1, -1, -1, -1,
    };
    static constexpr size_t jcc_opcode_index = 16;

    auto* module_base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    if (!module_base) {
        return false;
    }

    auto* dos_hdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt_hdr = reinterpret_cast<const IMAGE_NT_HEADERS*>(module_base + dos_hdr->e_lfanew);
    if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    uintptr_t match_addr = find_pattern(module_base, nt_hdr->OptionalHeader.SizeOfImage, pattern);
    if (!match_addr) {
        return false;
    }

    auto jcc_addr = match_addr + jcc_opcode_index;
    auto current_opcode = addr_as_ref<uint8_t>(jcc_addr);
    if (current_opcode != expected_jcc_opcode) {
        return false;
    }

    write_mem<uint8_t>(jcc_addr, 0xEB); // jmp short
    xlog::info("Disabled RF2 video memory requirement check at 0x{:x}", jcc_addr);
    return true;
}

using Direct3DCreate8Fn = IDirect3D8*(__stdcall*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(
    IDirect3D8*,
    UINT,
    D3DDEVTYPE,
    HWND,
    DWORD,
    D3DPRESENT_PARAMETERS*,
    IDirect3DDevice8**);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ShowWindowFn = BOOL(__stdcall*)(HWND, int);
using ShowWindowAsyncFn = BOOL(__stdcall*)(HWND, int);
using SetWindowPosFn = BOOL(__stdcall*)(HWND, HWND, int, int, int, int, UINT);
using SetWindowLongFn = LONG(__stdcall*)(HWND, int, LONG);
using SetThreadPriorityFn = BOOL(__stdcall*)(HANDLE, int);
using SleepFn = VOID(__stdcall*)(DWORD);
using SetVideoModeFn = int(__cdecl*)(int, int, int, int, int, HWND, int, int, int, int);
using GetDimensionFn = int(__cdecl*)();
using IsWindowActiveFn = uint8_t(__cdecl*)();
using InitMouseSurfaceFn = void(__cdecl*)(int, int);
using ResetInputStateFn = void(__cdecl*)();
using PlayMovieFn = int(__cdecl*)(const char*, int);

bool g_windowed_mode_forced = false;
HWND g_game_window = nullptr;
CreateDeviceFn g_original_create_device = nullptr;
ResetFn g_original_reset = nullptr;
PresentFn g_original_present = nullptr;
ShowWindowFn g_original_show_window = nullptr;
ShowWindowAsyncFn g_original_show_window_async = nullptr;
SetWindowPosFn g_original_set_window_pos = nullptr;
SetWindowLongFn g_original_set_window_long_a = nullptr;
SetWindowLongFn g_original_set_window_long_w = nullptr;
int g_video_mode_log_count = 0;
int g_set_window_pos_log_count = 0;
int g_mouse_clip_log_count = 0;
int g_mouse_surface_log_count = 0;
std::atomic<int> g_thread_priority_log_count{0};
std::atomic<int> g_sleep_clamp_log_count{0};
int g_active_override_log_count = 0;
int g_input_flush_log_count = 0;
int g_fast_start_log_count = 0;
int g_direct_input_log_count = 0;
int g_aim_slowdown_log_count = 0;
int g_crosshair_indicator_log_count = 0;
std::array<uintptr_t, 16> g_inactive_callsite_rvas{};
size_t g_inactive_callsite_count = 0;
float g_orig_aa_slowdown_min = 0.0f;
float g_orig_aa_slowdown_max = 0.0f;
bool g_orig_aa_values_captured = false;
std::array<uintptr_t, rf2::player::reticle::enemy_variant_pairs.size()> g_orig_enemy_reticle_ptrs{};
bool g_orig_enemy_reticle_ptrs_captured = false;

int __cdecl set_video_mode_hook(
    int width,
    int height,
    int color_depth,
    int renderer_id,
    int window_mode,
    HWND hwnd,
    int arg18,
    int y,
    int arg20,
    int arg24);
void __cdecl init_mouse_surface_hook(int width, int height);
int __cdecl play_movie_hook(const char* movie_name, int mode);
void __cdecl mouse_update_hook();

FunHook<int __cdecl(int, int, int, int, int, HWND, int, int, int, int)> g_set_video_mode_hook{
    rf2::gr::set_video_mode_addr,
    set_video_mode_hook,
};
int __cdecl get_width_hook();
int __cdecl get_height_hook();
int __cdecl get_viewport_width_hook();
int __cdecl get_viewport_height_hook();
uint8_t __cdecl is_window_active_hook();
BOOL __stdcall set_thread_priority_hook(HANDLE thread, int priority);
VOID __stdcall sleep_hook(DWORD milliseconds);
FunHook<int __cdecl()> g_get_width_hook{
    rf2::gr::get_width_addr,
    get_width_hook,
};
FunHook<int __cdecl()> g_get_height_hook{
    rf2::gr::get_height_addr,
    get_height_hook,
};
FunHook<int __cdecl()> g_get_viewport_width_hook{
    rf2::gr::get_viewport_width_addr,
    get_viewport_width_hook,
};
FunHook<int __cdecl()> g_get_viewport_height_hook{
    rf2::gr::get_viewport_height_addr,
    get_viewport_height_hook,
};
FunHook<uint8_t __cdecl()> g_is_window_active_hook{
    rf2::gr::is_window_active_addr,
    is_window_active_hook,
};
SetThreadPriorityFn g_original_set_thread_priority = nullptr;
FunHook<BOOL __stdcall(HANDLE, int)> g_set_thread_priority_hook{
    static_cast<uintptr_t>(0),
    set_thread_priority_hook,
};
FunHook<VOID __stdcall(DWORD)> g_sleep_hook{
    static_cast<uintptr_t>(0),
    sleep_hook,
};
FunHook<void __cdecl(int, int)> g_init_mouse_surface_hook{
    rf2::gr::init_mouse_surface_addr,
    init_mouse_surface_hook,
};
FunHook<int __cdecl(const char*, int)> g_play_movie_hook{
    rf2::gr::play_movie_addr,
    play_movie_hook,
};
FunHook<void __cdecl()> g_mouse_update_hook{
    rf2::os::input::mouse_update_addr,
    mouse_update_hook,
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

bool is_address_in_rf2_module(uintptr_t address)
{
    static const uintptr_t base = rf2::module_base();
    static const uintptr_t end = [] {
        auto* module_base = reinterpret_cast<const uint8_t*>(rf2::module_base());
        if (!module_base) {
            return rf2::module_base();
        }

        auto* dos_hdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
        if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
            return rf2::module_base();
        }

        auto* nt_hdr = reinterpret_cast<const IMAGE_NT_HEADERS*>(module_base + dos_hdr->e_lfanew);
        if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
            return rf2::module_base();
        }
        return base + static_cast<uintptr_t>(nt_hdr->OptionalHeader.SizeOfImage);
    }();

    return address >= base && address < end;
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
    if (is_current_process_top_level_window(g_game_window)) {
        return g_game_window;
    }
    return find_best_process_window();
}

void get_forced_window_position(int& x, int& y)
{
    if (g_borderless_mode) {
        x = g_forced_window_x;
        y = g_forced_window_y;
        return;
    }

    RECT work_area{};
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work_area, 0)) {
        x = work_area.left + g_forced_window_x;
        y = work_area.top + g_forced_window_y;
    }
    else {
        x = g_forced_window_x;
        y = g_forced_window_y;
    }
}

void sync_resolution_globals()
{
    if (!g_force_resolution) {
        return;
    }

    // RF2 uses multiple globals for viewport/input limits.
    rf2::gr::window_width = static_cast<int>(g_forced_window_width);
    rf2::gr::window_height = static_cast<int>(g_forced_window_height);
    rf2::gr::clip_width = static_cast<int>(g_forced_window_width);
    rf2::gr::clip_height = static_cast<int>(g_forced_window_height);
    rf2::gr::clip_right = static_cast<int>(g_forced_window_width) - 1;
    rf2::gr::clip_bottom = static_cast<int>(g_forced_window_height) - 1;
    rf2::gr::mouse_limit_x = static_cast<int>(g_forced_window_width) - 1;
    rf2::gr::mouse_limit_y = static_cast<int>(g_forced_window_height) - 1;
    rf2::gr::mouse_surface_width = static_cast<int>(g_forced_window_width);
    rf2::gr::mouse_surface_height = static_cast<int>(g_forced_window_height);
    // RF2 stores aspect as (width / height) * 0.75 (4:3 normalization factor).
    rf2::gr::aspect_4_3_scale =
        (static_cast<float>(g_forced_window_width) / static_cast<float>(g_forced_window_height)) * 0.75f;
    rf2::gr::backbuffer_width = static_cast<int>(g_forced_window_width);
    rf2::gr::backbuffer_height = static_cast<int>(g_forced_window_height);
}

void update_mouse_clip(HWND window)
{
    (void)window;
    // Disabled for now: clipping can break alt-tab and cause input stalls on some systems.
    ClipCursor(nullptr);
}

void make_present_parameters_windowed(D3DPRESENT_PARAMETERS* params)
{
    if (!g_force_window_mode || !params) {
        return;
    }
    params->BackBufferWidth = g_forced_window_width;
    params->BackBufferHeight = g_forced_window_height;
    params->Windowed = TRUE;
    params->FullScreen_RefreshRateInHz = 0;
    // DEFAULT is more stable for RF2 in windowed/borderless mode.
    params->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    g_windowed_mode_forced = true;
}

void force_windowed_style(HWND window, UINT client_width, UINT client_height, bool activate_window)
{
    if (!g_force_window_mode) {
        return;
    }

    (void)client_width;
    (void)client_height;
    window = resolve_target_window(window);
    if (!window) {
        return;
    }
    g_game_window = window;
    console_attach_to_window(window);
    sync_resolution_globals();

    const UINT width = g_forced_window_width;
    const UINT height = g_forced_window_height;

    LONG style = GetWindowLongA(window, GWL_STYLE);
    LONG ex_style = GetWindowLongA(window, GWL_EXSTYLE);

    int window_width = static_cast<int>(width);
    int window_height = static_cast<int>(height);
    if (g_borderless_mode) {
        style &= ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_POPUP | WS_VISIBLE;
        ex_style &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        ex_style |= WS_EX_APPWINDOW;
    }
    else {
        style &= ~WS_POPUP;
        style |= WS_OVERLAPPEDWINDOW;
        ex_style &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        ex_style |= WS_EX_APPWINDOW;

        RECT window_rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRectEx(&window_rect, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style));
        window_width = window_rect.right - window_rect.left;
        window_height = window_rect.bottom - window_rect.top;
    }

    SetWindowLongA(window, GWL_STYLE, style);
    SetWindowLongA(window, GWL_EXSTYLE, ex_style);
    SetWindowLongA(window, GWL_HWNDPARENT, 0);

    int x = 0;
    int y = 0;
    get_forced_window_position(x, y);
    SetWindowPos(
        window,
        HWND_NOTOPMOST,
        x,
        y,
        window_width,
        window_height,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

    if (activate_window) {
        ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        SetActiveWindow(window);
        SetFocus(window);
        UpdateWindow(window);
    }
    else {
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    update_mouse_clip(window);
}

bool patch_vtable_entry(void* instance, size_t index, void* replacement, void** original_out)
{
    if (!instance) {
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(instance);
    void** slot = &vtable[index];
    if (*slot == replacement) {
        return true;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        xlog::warn("VirtualProtect failed while patching vtable");
        return false;
    }

    if (original_out) {
        *original_out = *slot;
    }
    *slot = replacement;

    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &dummy);
    return true;
}

HRESULT __stdcall d3d8_reset_hook(IDirect3DDevice8* self, D3DPRESENT_PARAMETERS* params)
{
    frame_limiter_on_device_reset();
    make_present_parameters_windowed(params);
    sync_resolution_globals();
    D3DDEVICE_CREATION_PARAMETERS creation_params{};
    HWND focus_window = nullptr;
    if (self && SUCCEEDED(self->GetCreationParameters(&creation_params))) {
        focus_window = creation_params.hFocusWindow;
        force_windowed_style(
            focus_window,
            params ? params->BackBufferWidth : 0,
            params ? params->BackBufferHeight : 0,
            false);
    }

    HRESULT hr = g_original_reset ? g_original_reset(self, params) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) {
        frame_limiter_on_device_reset();
        force_windowed_style(
            focus_window,
            params ? params->BackBufferWidth : 0,
            params ? params->BackBufferHeight : 0,
            false);
    }
    return hr;
}

HRESULT __stdcall d3d8_present_hook(
    IDirect3DDevice8* self,
    const RECT* src_rect,
    const RECT* dst_rect,
    HWND dst_window_override,
    const RGNDATA* dirty_region)
{
    frame_limiter_on_present();
    const HRESULT hr = g_original_present
        ? g_original_present(self, src_rect, dst_rect, dst_window_override, dirty_region)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) {
        HWND overlay_window = resolve_target_window(dst_window_override);
        if (!overlay_window && self) {
            D3DDEVICE_CREATION_PARAMETERS creation_params{};
            if (SUCCEEDED(self->GetCreationParameters(&creation_params))) {
                overlay_window = resolve_target_window(creation_params.hFocusWindow);
            }
        }
        frame_limiter_draw_overlay(overlay_window);
    }
    return hr;
}

void hook_d3d8_device(IDirect3DDevice8* device)
{
    if (!device) {
        return;
    }
    patch_vtable_entry(
        device,
        d3d8_device_reset_vtbl_index,
        reinterpret_cast<void*>(d3d8_reset_hook),
        reinterpret_cast<void**>(&g_original_reset));
    if (frame_limiter_is_active()) {
        patch_vtable_entry(
            device,
            d3d8_device_present_vtbl_index,
            reinterpret_cast<void*>(d3d8_present_hook),
            reinterpret_cast<void**>(&g_original_present));
    }
}

HRESULT __stdcall d3d8_create_device_hook(
    IDirect3D8* self,
    UINT adapter,
    D3DDEVTYPE device_type,
    HWND focus_window,
    DWORD behavior_flags,
    D3DPRESENT_PARAMETERS* params,
    IDirect3DDevice8** out_device)
{
    make_present_parameters_windowed(params);
    sync_resolution_globals();
    force_windowed_style(
        focus_window,
        params ? params->BackBufferWidth : 0,
        params ? params->BackBufferHeight : 0,
        true);

    HRESULT hr = g_original_create_device
        ? g_original_create_device(self, adapter, device_type, focus_window, behavior_flags, params, out_device)
        : D3DERR_INVALIDCALL;

    if (SUCCEEDED(hr) && out_device && *out_device) {
        hook_d3d8_device(*out_device);
        D3DDEVICE_CREATION_PARAMETERS creation_params{};
        if (SUCCEEDED((*out_device)->GetCreationParameters(&creation_params))) {
            force_windowed_style(
                creation_params.hFocusWindow,
                params ? params->BackBufferWidth : 0,
                params ? params->BackBufferHeight : 0,
                true);
        }
    }
    return hr;
}

int __cdecl set_video_mode_hook(
    int width,
    int height,
    int color_depth,
    int renderer_id,
    int window_mode,
    HWND hwnd,
    int arg18,
    int y,
    int arg20,
    int arg24)
{
    if (hwnd) {
        console_attach_to_window(hwnd);
    }
    frame_limiter_apply_runtime_overrides();

    if (!g_force_window_mode) {
        const int target_width = static_cast<int>(g_forced_window_width);
        const int target_height = static_cast<int>(g_forced_window_height);
        if (g_video_mode_log_count < 16) {
            ++g_video_mode_log_count;
            xlog::info(
                "RF2 video mode request (fullscreen): {}x{} mode=0x{:X} -> forcing {}x{} mode=0x{:X}",
                width,
                height,
                window_mode,
                target_width,
                target_height,
                window_mode);
        }
        const int result = g_set_video_mode_hook.call_target(
            target_width,
            target_height,
            color_depth,
            renderer_id,
            window_mode,
            hwnd,
            arg18,
            y,
            arg20,
            arg24);
        camera_set_resolution(static_cast<unsigned>(std::max(target_width, 1)), static_cast<unsigned>(std::max(target_height, 1)));
        frame_limiter_apply_runtime_overrides();
        return result;
    }

    g_windowed_mode_forced = true;

    if (g_video_mode_log_count < 16) {
        ++g_video_mode_log_count;
        xlog::info(
            "RF2 video mode request: {}x{} mode=0x{:X} -> forcing {}x{} mode=0x{:X}",
            width,
            height,
            window_mode,
            g_forced_window_width,
            g_forced_window_height,
            g_forced_window_mode);
    }

    int result = g_set_video_mode_hook.call_target(
        static_cast<int>(g_forced_window_width),
        static_cast<int>(g_forced_window_height),
        color_depth,
        renderer_id,
        g_forced_window_mode,
        hwnd,
        arg18,
        y,
        arg20,
        arg24);

    // Keep RF2 render globals aligned with forced mode.
    sync_resolution_globals();
    camera_set_resolution(g_forced_window_width, g_forced_window_height);
    frame_limiter_apply_runtime_overrides();
    rf2::gr::window_mode = g_forced_window_mode;

    force_windowed_style(hwnd, g_forced_window_width, g_forced_window_height, true);
    return result;
}

void __cdecl init_mouse_surface_hook(int width, int height)
{
    if (!g_force_resolution) {
        g_init_mouse_surface_hook.call_target(width, height);
        return;
    }

    if (g_mouse_surface_log_count < 16) {
        ++g_mouse_surface_log_count;
        xlog::info(
            "RF2 mouse surface init: {}x{} -> forcing {}x{}",
            width,
            height,
            g_forced_window_width,
            g_forced_window_height);
    }

    g_init_mouse_surface_hook.call_target(static_cast<int>(g_forced_window_width), static_cast<int>(g_forced_window_height));
    sync_resolution_globals();
}

int __cdecl get_width_hook()
{
    if (!g_force_resolution) {
        return g_get_width_hook.call_target();
    }
    sync_resolution_globals();
    return static_cast<int>(g_forced_window_width);
}

int __cdecl get_height_hook()
{
    if (!g_force_resolution) {
        return g_get_height_hook.call_target();
    }
    sync_resolution_globals();
    return static_cast<int>(g_forced_window_height);
}

int __cdecl get_viewport_width_hook()
{
    if (!g_force_resolution) {
        return g_get_viewport_width_hook.call_target();
    }
    sync_resolution_globals();
    return static_cast<int>(g_forced_window_width);
}

int __cdecl get_viewport_height_hook()
{
    if (!g_force_resolution) {
        return g_get_viewport_height_hook.call_target();
    }
    sync_resolution_globals();
    return static_cast<int>(g_forced_window_height);
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

std::string_view movie_basename(std::string_view movie_name)
{
    const size_t slash_pos = movie_name.find_last_of("\\/");
    if (slash_pos == std::string_view::npos) {
        return movie_name;
    }
    return movie_name.substr(slash_pos + 1);
}

bool iequals_ascii(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        unsigned char lc = static_cast<unsigned char>(left[i]);
        unsigned char rc = static_cast<unsigned char>(right[i]);
        if (std::tolower(lc) != std::tolower(rc)) {
            return false;
        }
    }
    return true;
}

bool parse_bool_like(std::string_view value, bool& out_value)
{
    const std::string lowered = to_lower_ascii_copy(trim_ascii_copy(std::string{value}));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        out_value = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        out_value = false;
        return true;
    }
    return false;
}

void save_direct_input_mouse_setting()
{
    if (g_settings.settings_file_path.empty()) {
        return;
    }
    if (!WritePrivateProfileStringA(
            "sopot",
            "direct_input_mouse",
            g_direct_input_mouse_enabled ? "1" : "0",
            g_settings.settings_file_path.c_str()))
    {
        xlog::warn(
            "Failed to persist direct_input_mouse={} to {}",
            g_direct_input_mouse_enabled ? 1 : 0,
            g_settings.settings_file_path);
    }
}

void apply_direct_input_mouse_mode(bool log_change)
{
    if (rf2::os::input::mouse_system_initialized == 0) {
        return;
    }

    if (g_direct_input_mouse_enabled) {
        if (!rf2::os::input::mouse_device && rf2::os::input::direct_input_api) {
            const int init_result = rf2::os::input::mouse_init_direct_input();
            if (log_change && g_direct_input_log_count < 12) {
                ++g_direct_input_log_count;
                xlog::info("DirectInput mouse init attempt returned {}", init_result);
            }
        }

        if (rf2::os::input::mouse_device) {
            rf2::os::input::mouse_direct_input_enabled = 1;
            return;
        }

        rf2::os::input::mouse_direct_input_enabled = 0;
        if (log_change) {
            xlog::warn("DirectInput mouse requested but RF2 mouse device is unavailable");
        }
        return;
    }

    if (rf2::os::input::mouse_device) {
        rf2::os::input::mouse_release_direct_input();
    }
    rf2::os::input::mouse_direct_input_enabled = 0;
    if (log_change && g_direct_input_log_count < 12) {
        ++g_direct_input_log_count;
        xlog::info("DirectInput mouse disabled");
    }
}

void save_aim_slowdown_setting()
{
    if (g_settings.settings_file_path.empty()) {
        return;
    }
    if (!WritePrivateProfileStringA(
            "sopot",
            "aim_slowdown_on_target",
            g_aim_slowdown_on_target_enabled ? "1" : "0",
            g_settings.settings_file_path.c_str()))
    {
        xlog::warn(
            "Failed to persist aim_slowdown_on_target={} to {}",
            g_aim_slowdown_on_target_enabled ? 1 : 0,
            g_settings.settings_file_path);
    }
}

void save_crosshair_enemy_indicator_setting()
{
    if (g_settings.settings_file_path.empty()) {
        return;
    }
    if (!WritePrivateProfileStringA(
            "sopot",
            "crosshair_enemy_indicator",
            g_crosshair_enemy_indicator_enabled ? "1" : "0",
            g_settings.settings_file_path.c_str()))
    {
        xlog::warn(
            "Failed to persist crosshair_enemy_indicator={} to {}",
            g_crosshair_enemy_indicator_enabled ? 1 : 0,
            g_settings.settings_file_path);
    }
}

void apply_aim_slowdown_setting(bool log_change)
{
    if (!g_orig_aa_values_captured) {
        g_orig_aa_slowdown_min = rf2::player::autoaim::slowdown_factor_min;
        g_orig_aa_slowdown_max = rf2::player::autoaim::slowdown_factor_max;
        g_orig_aa_values_captured = true;
    }

    if (g_aim_slowdown_on_target_enabled) {
        rf2::player::autoaim::slowdown_factor_min = g_orig_aa_slowdown_min;
        rf2::player::autoaim::slowdown_factor_max = g_orig_aa_slowdown_max;
        if (log_change && g_aim_slowdown_log_count < 8) {
            ++g_aim_slowdown_log_count;
            xlog::info(
                "Aim slowdown enabled (aa_slowdown_factor_min={}, aa_slowdown_factor_max={})",
                g_orig_aa_slowdown_min,
                g_orig_aa_slowdown_max);
        }
        return;
    }

    rf2::player::autoaim::slowdown_factor_min = 1.0f;
    rf2::player::autoaim::slowdown_factor_max = 1.0f;
    if (log_change && g_aim_slowdown_log_count < 8) {
        ++g_aim_slowdown_log_count;
        xlog::info("Aim slowdown disabled (aa_slowdown_factor_min/max forced to 1.0)");
    }
}

void apply_crosshair_enemy_indicator_setting(bool log_change)
{
    if (!g_orig_enemy_reticle_ptrs_captured) {
        for (size_t i = 0; i < rf2::player::reticle::enemy_variant_pairs.size(); ++i) {
            const auto& pair = rf2::player::reticle::enemy_variant_pairs[i];
            g_orig_enemy_reticle_ptrs[i] = addr_as_ref<uintptr_t>(pair.enemy_ptr_addr);
        }
        g_orig_enemy_reticle_ptrs_captured = true;
    }

    if (g_crosshair_enemy_indicator_enabled) {
        for (size_t i = 0; i < rf2::player::reticle::enemy_variant_pairs.size(); ++i) {
            const auto& pair = rf2::player::reticle::enemy_variant_pairs[i];
            addr_as_ref<uintptr_t>(pair.enemy_ptr_addr) = g_orig_enemy_reticle_ptrs[i];
        }
        if (log_change && g_crosshair_indicator_log_count < 8) {
            ++g_crosshair_indicator_log_count;
            xlog::info("Enemy crosshair indicator enabled");
        }
        return;
    }

    for (const auto& pair : rf2::player::reticle::enemy_variant_pairs) {
        addr_as_ref<uintptr_t>(pair.enemy_ptr_addr) = addr_as_ref<uintptr_t>(pair.normal_ptr_addr);
    }
    if (log_change && g_crosshair_indicator_log_count < 8) {
        ++g_crosshair_indicator_log_count;
        xlog::info("Enemy crosshair indicator disabled (enemy reticle variants remapped)");
    }
}

bool is_fast_start_logo_movie(const char* movie_name)
{
    if (!movie_name) {
        return false;
    }

    const std::string_view name = movie_basename(movie_name);
    return iequals_ascii(name, "thq-v.bik")
        || iequals_ascii(name, "volition-logo.bik")
        || iequals_ascii(name, "outrage-logo.bik");
}

int __cdecl play_movie_hook(const char* movie_name, int mode)
{
    if (g_fast_start_enabled && is_fast_start_logo_movie(movie_name)) {
        if (g_fast_start_log_count < 12) {
            ++g_fast_start_log_count;
            xlog::info("Fast start: skipping startup movie {} (mode {})", movie_name ? movie_name : "<null>", mode);
        }
        return 1;
    }

    return g_play_movie_hook.call_target(movie_name, mode);
}

void __cdecl mouse_update_hook()
{
    apply_direct_input_mouse_mode(false);
    apply_aim_slowdown_setting(false);
    g_mouse_update_hook.call_target();
}

bool is_game_window_foreground()
{
    HWND game_root = resolve_top_level_window(g_game_window);
    if (!game_root) {
        return false;
    }

    HWND foreground_root = resolve_top_level_window(GetForegroundWindow());
    if (foreground_root && foreground_root == game_root) {
        return true;
    }

    HWND active_root = resolve_top_level_window(GetActiveWindow());
    return active_root && active_root == game_root;
}

uint8_t __cdecl is_window_active_hook()
{
    frame_limiter_apply_runtime_overrides();
    const uint8_t active = g_is_window_active_hook.call_target();
    if (active) {
        return active;
    }

    // Keep gameplay timing path consistent while SOPOT's in-game console has focus.
    if (console_is_open()) {
        return 1;
    }
    if (is_game_window_foreground()) {
        return 1;
    }

    const uintptr_t caller_va = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t base = rf2::module_base();
    const uintptr_t caller_rva = caller_va - base;

    // Only bypass inactive gating in known wait loops.
    switch (caller_rva) {
    case rf2::os::window::inactive_wait_caller_rva_1:
    case rf2::os::window::inactive_wait_caller_rva_2:
    case rf2::os::window::inactive_wait_caller_rva_3:
    case rf2::os::window::inactive_wait_caller_rva_4:
    case rf2::os::window::inactive_wait_caller_rva_5:
    case rf2::os::window::inactive_wait_caller_rva_6:
    case rf2::os::window::inactive_wait_caller_rva_7:
        if (g_active_override_log_count < 12) {
            ++g_active_override_log_count;
            xlog::info(
                "Bypassing RF2 inactive wait loop at caller rva=0x{:X} va=0x{:X}",
                static_cast<unsigned>(caller_rva),
                static_cast<unsigned>(caller_va));
        }
        return 1;
    default:
        bool seen = false;
        for (size_t i = 0; i < g_inactive_callsite_count; ++i) {
            if (g_inactive_callsite_rvas[i] == caller_rva) {
                seen = true;
                break;
            }
        }
        if (!seen && g_inactive_callsite_count < g_inactive_callsite_rvas.size()) {
            g_inactive_callsite_rvas[g_inactive_callsite_count++] = caller_rva;
            xlog::info(
                "RF2 inactive state observed at unhandled caller rva=0x{:X} va=0x{:X}",
                static_cast<unsigned>(caller_rva),
                static_cast<unsigned>(caller_va));
        }
        return 0;
    }
}

void flush_input_state_on_deactivate()
{
    // RF2 misses key-up transitions on focus loss; flush key/edge state to avoid latched Alt/Tab.
    rf2::os::input::reset_key_state();
    rf2::os::input::reset_edge_state();
    rf2::os::input::alt_key_down = 0;
    rf2::os::input::tab_key_down = 0;

    if (g_input_flush_log_count < 8) {
        ++g_input_flush_log_count;
        xlog::info("Flushed RF2 input state on focus loss");
    }
}

BOOL __stdcall set_thread_priority_hook(HANDLE thread, int priority)
{
    const uintptr_t caller_va = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool caller_in_rf2 = is_address_in_rf2_module(caller_va);
    int patched_priority = priority;
    if (caller_in_rf2 && patched_priority < THREAD_PRIORITY_NORMAL) {
        flush_input_state_on_deactivate();
        patched_priority = THREAD_PRIORITY_NORMAL;
        const int log_idx = g_thread_priority_log_count.fetch_add(1, std::memory_order_relaxed);
        if (log_idx < 8) {
            xlog::info(
                "Clamping SetThreadPriority from {} to {} to avoid background throttling",
                priority,
                patched_priority);
        }
    }
    return g_original_set_thread_priority ? g_original_set_thread_priority(thread, patched_priority) : FALSE;
}

VOID __stdcall sleep_hook(DWORD milliseconds)
{
    struct Guard
    {
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    };
    thread_local bool in_sleep_hook = false;
    if (in_sleep_hook) {
        g_sleep_hook.call_target(milliseconds);
        return;
    }
    Guard guard{in_sleep_hook};

    const uintptr_t caller_va = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!is_address_in_rf2_module(caller_va)) {
        g_sleep_hook.call_target(milliseconds);
        return;
    }

    DWORD patched_milliseconds = milliseconds;
    if (milliseconds > 0 && milliseconds <= 20) {
        // Keep RF2's active frametime pacing intact. Only bypass short sleeps when
        // the game is inactive to avoid background throttling/freezes on alt-tab.
        const bool is_active = g_is_window_active_hook.call_target() != 0;
        if (!is_active) {
            patched_milliseconds = 0;
            const int log_idx = g_sleep_clamp_log_count.fetch_add(1, std::memory_order_relaxed);
            if (log_idx < 16) {
                const uintptr_t caller_rva = caller_va - rf2::module_base();
                xlog::info(
                    "Clamping inactive Sleep from {}ms to 0ms at caller rva=0x{:X} va=0x{:X}",
                    static_cast<unsigned>(milliseconds),
                    static_cast<unsigned>(caller_rva),
                    static_cast<unsigned>(caller_va));
            }
        }
    }
    g_sleep_hook.call_target(patched_milliseconds);
}

bool should_patch_window(HWND window)
{
    if (!g_force_window_mode || !g_windowed_mode_forced) {
        return false;
    }
    HWND root = resolve_top_level_window(window);
    if (!root) {
        return false;
    }
    if (!g_game_window) {
        return true;
    }
    return root == g_game_window;
}

int remap_show_cmd_forced_window(int cmd_show)
{
    switch (cmd_show) {
    case SW_HIDE:
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
    case SW_FORCEMINIMIZE:
        return SW_RESTORE;
    default:
        return cmd_show;
    }
}

BOOL __stdcall show_window_hook(HWND window, int cmd_show)
{
    if (should_patch_window(window)) {
        cmd_show = remap_show_cmd_forced_window(cmd_show);
    }
    return g_original_show_window ? g_original_show_window(window, cmd_show) : FALSE;
}

BOOL __stdcall show_window_async_hook(HWND window, int cmd_show)
{
    if (should_patch_window(window)) {
        cmd_show = remap_show_cmd_forced_window(cmd_show);
    }
    return g_original_show_window_async ? g_original_show_window_async(window, cmd_show) : FALSE;
}

BOOL __stdcall set_window_pos_hook(HWND window, HWND insert_after, int x, int y, int cx, int cy, UINT flags)
{
    if (should_patch_window(window)) {
        const UINT original_flags = flags;
        const HWND original_insert_after = insert_after;
        const int original_x = x;
        const int original_y = y;
        const int original_cx = cx;
        const int original_cy = cy;

        if ((flags & SWP_HIDEWINDOW) != 0) {
            flags &= ~SWP_HIDEWINDOW;
            flags |= SWP_SHOWWINDOW;
        }

        int forced_x = 0;
        int forced_y = 0;
        get_forced_window_position(forced_x, forced_y);
        flags &= ~SWP_NOMOVE;
        flags &= ~SWP_NOSIZE;
        flags &= ~SWP_NOACTIVATE;
        flags &= ~SWP_NOZORDER;
        insert_after = HWND_TOP;
        x = forced_x;
        y = forced_y;
        cx = static_cast<int>(g_forced_window_width);
        cy = static_cast<int>(g_forced_window_height);

        if (g_set_window_pos_log_count < 24) {
            ++g_set_window_pos_log_count;
            xlog::info(
                "SetWindowPos override hwnd={} z={} x/y {}x{} -> {}x{} size {}x{} -> {}x{} flags 0x{:X} -> 0x{:X}",
                static_cast<void*>(window),
                static_cast<void*>(original_insert_after),
                original_x,
                original_y,
                x,
                y,
                original_cx,
                original_cy,
                cx,
                cy,
                original_flags,
                flags);
        }

        BOOL result = g_original_set_window_pos
            ? g_original_set_window_pos(window, insert_after, x, y, cx, cy, flags)
            : FALSE;
        RECT actual_rect{};
        bool got_rect = GetWindowRect(window, &actual_rect) == TRUE;
        if (g_set_window_pos_log_count < 48) {
            ++g_set_window_pos_log_count;
            xlog::info(
                "SetWindowPos result hwnd={} ok={} gle={} actual={}x{}+{},{}",
                static_cast<void*>(window),
                result ? 1 : 0,
                result ? 0 : static_cast<int>(GetLastError()),
                got_rect ? (actual_rect.right - actual_rect.left) : -1,
                got_rect ? (actual_rect.bottom - actual_rect.top) : -1,
                got_rect ? actual_rect.left : -1,
                got_rect ? actual_rect.top : -1);
        }
        ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        SetActiveWindow(window);
        SetFocus(window);
        return result;
    }
    return g_original_set_window_pos ? g_original_set_window_pos(window, insert_after, x, y, cx, cy, flags) : FALSE;
}

LONG sanitize_window_long_value(int index, LONG new_long)
{
    if (index == GWL_STYLE) {
        if (g_borderless_mode) {
            new_long &= ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            new_long |= WS_POPUP | WS_VISIBLE;
        }
        else {
            new_long &= ~WS_POPUP;
            new_long |= WS_OVERLAPPEDWINDOW;
        }
    }
    else if (index == GWL_EXSTYLE) {
        new_long &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        new_long |= WS_EX_APPWINDOW;
    }
    else if (index == GWL_HWNDPARENT) {
        new_long = 0;
    }
    return new_long;
}

LONG __stdcall set_window_long_a_hook(HWND window, int index, LONG new_long)
{
    if (should_patch_window(window)) {
        new_long = sanitize_window_long_value(index, new_long);
    }
    return g_original_set_window_long_a ? g_original_set_window_long_a(window, index, new_long) : 0;
}

LONG __stdcall set_window_long_w_hook(HWND window, int index, LONG new_long)
{
    if (should_patch_window(window)) {
        new_long = sanitize_window_long_value(index, new_long);
    }
    return g_original_set_window_long_w ? g_original_set_window_long_w(window, index, new_long) : 0;
}

FunHook<BOOL __stdcall(HWND, int)> g_show_window_hook{
    static_cast<uintptr_t>(0),
    show_window_hook,
};
FunHook<BOOL __stdcall(HWND, int)> g_show_window_async_hook{
    static_cast<uintptr_t>(0),
    show_window_async_hook,
};
FunHook<BOOL __stdcall(HWND, HWND, int, int, int, int, UINT)> g_set_window_pos_hook{
    static_cast<uintptr_t>(0),
    set_window_pos_hook,
};
FunHook<LONG __stdcall(HWND, int, LONG)> g_set_window_long_a_hook{
    static_cast<uintptr_t>(0),
    set_window_long_a_hook,
};
FunHook<LONG __stdcall(HWND, int, LONG)> g_set_window_long_w_hook{
    static_cast<uintptr_t>(0),
    set_window_long_w_hook,
};

void install_show_window_hooks()
{
    HMODULE user32_module = GetModuleHandleA("user32.dll");
    if (!user32_module) {
        user32_module = LoadLibraryA("user32.dll");
    }
    if (!user32_module) {
        xlog::warn("Failed to load user32.dll for window hooks");
        return;
    }

    auto* show_window_addr = reinterpret_cast<ShowWindowFn>(GetProcAddress(user32_module, "ShowWindow"));
    auto* show_window_async_addr = reinterpret_cast<ShowWindowAsyncFn>(GetProcAddress(user32_module, "ShowWindowAsync"));
    auto* set_window_pos_addr = reinterpret_cast<SetWindowPosFn>(GetProcAddress(user32_module, "SetWindowPos"));
    auto* set_window_long_a_addr = reinterpret_cast<SetWindowLongFn>(GetProcAddress(user32_module, "SetWindowLongA"));
    auto* set_window_long_w_addr = reinterpret_cast<SetWindowLongFn>(GetProcAddress(user32_module, "SetWindowLongW"));
    if (!show_window_addr || !show_window_async_addr || !set_window_pos_addr || !set_window_long_a_addr || !set_window_long_w_addr) {
        xlog::warn("Could not resolve one or more user32 window exports");
        return;
    }

    g_show_window_hook.set_addr(reinterpret_cast<uintptr_t>(show_window_addr));
    g_show_window_hook.install();
    g_show_window_async_hook.set_addr(reinterpret_cast<uintptr_t>(show_window_async_addr));
    g_show_window_async_hook.install();
    g_set_window_pos_hook.set_addr(reinterpret_cast<uintptr_t>(set_window_pos_addr));
    g_set_window_pos_hook.install();
    g_set_window_long_a_hook.set_addr(reinterpret_cast<uintptr_t>(set_window_long_a_addr));
    g_set_window_long_a_hook.install();
    g_set_window_long_w_hook.set_addr(reinterpret_cast<uintptr_t>(set_window_long_w_addr));
    g_set_window_long_w_hook.install();
}

void install_background_activity_patch()
{
    static bool installed = false;
    if (installed) {
        return;
    }

    g_is_window_active_hook.install();

    HMODULE kernel32_module = GetModuleHandleA("kernel32.dll");
    if (!kernel32_module) {
        kernel32_module = LoadLibraryA("kernel32.dll");
    }
    if (!kernel32_module) {
        xlog::warn("Failed to load kernel32.dll for thread priority hook");
        installed = true;
        return;
    }

    auto* set_thread_priority_addr =
        reinterpret_cast<SetThreadPriorityFn>(GetProcAddress(kernel32_module, "SetThreadPriority"));
    if (set_thread_priority_addr) {
        g_set_thread_priority_hook.set_addr(reinterpret_cast<uintptr_t>(set_thread_priority_addr));
        g_set_thread_priority_hook.install();
    }
    else {
        xlog::warn("Could not resolve SetThreadPriority export");
    }

    auto* sleep_addr = reinterpret_cast<SleepFn>(GetProcAddress(kernel32_module, "Sleep"));
    if (sleep_addr) {
        g_sleep_hook.set_addr(reinterpret_cast<uintptr_t>(sleep_addr));
        g_sleep_hook.install();
    }
    else {
        xlog::warn("Could not resolve Sleep export");
    }

    installed = true;
    xlog::info("Installed background activity patch (inactive-wait bypass + thread priority/sleep clamps)");
}

IDirect3D8* __stdcall d3d8_create8_export_hook(UINT sdk_version);
FunHook<IDirect3D8* __stdcall(UINT)> g_d3d8_create8_export_hook{
    static_cast<uintptr_t>(0),
    d3d8_create8_export_hook,
};

void hook_d3d8_instance(IDirect3D8* d3d8)
{
    if (!d3d8) {
        return;
    }
    patch_vtable_entry(
        d3d8,
        d3d8_create_device_vtbl_index,
        reinterpret_cast<void*>(d3d8_create_device_hook),
        reinterpret_cast<void**>(&g_original_create_device));
}

IDirect3D8* __stdcall d3d8_create8_export_hook(UINT sdk_version)
{
    IDirect3D8* d3d8 = g_d3d8_create8_export_hook.call_target(sdk_version);
    hook_d3d8_instance(d3d8);
    return d3d8;
}

void install_window_mode_patch()
{
    static bool installed = false;
    if (installed) {
        return;
    }

    // Always hook RF2 set_video_mode so configured resolution applies even in fullscreen.
    g_set_video_mode_hook.install();
    g_get_width_hook.install();
    g_get_height_hook.install();
    g_get_viewport_width_hook.install();
    g_get_viewport_height_hook.install();
    g_init_mouse_surface_hook.install();
    g_mouse_update_hook.install();

    if (!g_force_window_mode) {
        install_background_activity_patch();
        installed = true;
        xlog::info("Installed video mode resolution hook (fullscreen setting)");
        return;
    }

    g_windowed_mode_forced = true;

    HMODULE d3d8_module = GetModuleHandleA("d3d8.dll");
    if (!d3d8_module) {
        d3d8_module = LoadLibraryA("d3d8.dll");
    }
    if (!d3d8_module) {
        xlog::warn("Failed to load d3d8.dll, windowed mode patch not installed");
        return;
    }

    auto* d3d8_create8_export = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8_module, "Direct3DCreate8"));
    if (!d3d8_create8_export) {
        xlog::warn("Could not resolve Direct3DCreate8 export");
        return;
    }

    g_d3d8_create8_export_hook.set_addr(reinterpret_cast<uintptr_t>(d3d8_create8_export));
    g_d3d8_create8_export_hook.install();
    install_background_activity_patch();
    // Keep user32 APIs unhooked for now; direct mode hook + explicit style/resize calls are more reliable.
    installed = true;
    xlog::info(
        "Installed window mode patch ({})",
        g_borderless_mode ? "borderless" : "windowed");
}

void disable_video_memory_requirement_check()
{
    static bool applied = false;
    if (applied) {
        return;
    }

    if (!patch_vram_check_opcode(0x7D) && !patch_vram_check_opcode(0x7C)) {
        xlog::warn("RF2 video memory requirement signature not found; check not patched");
        return;
    }

    applied = true;
}

void install_fast_start_patch()
{
    static bool installed = false;
    if (installed) {
        return;
    }

    if (!g_fast_start_enabled) {
        xlog::info("Fast start disabled by settings");
        return;
    }

    g_play_movie_hook.install();
    installed = true;
    xlog::info("Installed fast start patch (startup logo movies will be skipped)");
}

} // namespace

FunHook<bool(char*)> fix_launch_hook{
    0x00570CE0,
    [](char* a1) {
        return true;
    },
};

void misc_apply_patches(const Rf2PatchSettings& settings)
{
    g_settings = settings;
    g_fast_start_enabled = g_settings.fast_start;
    g_direct_input_mouse_enabled = g_settings.direct_input_mouse;
    g_aim_slowdown_on_target_enabled = g_settings.aim_slowdown_on_target;
    g_crosshair_enemy_indicator_enabled = g_settings.crosshair_enemy_indicator;

    fix_launch_hook.install();
    frame_limiter_apply_settings(g_settings);
    high_fps_apply_patch();
    camera_apply_settings(g_settings);
    configure_window_mode_settings();
    disable_video_memory_requirement_check();
    console_install_output_hook();
    install_fast_start_patch();
    install_window_mode_patch();
    apply_direct_input_mouse_mode(true);
    apply_aim_slowdown_setting(true);
    apply_crosshair_enemy_indicator_setting(true);
}

bool misc_try_handle_console_command(
    const std::string& command,
    bool& out_success,
    std::string& out_status,
    std::vector<std::string>& out_output_lines)
{
    out_success = false;
    out_status.clear();
    out_output_lines.clear();

    const std::string trimmed = trim_ascii_copy(command);

    auto handle_bool_command = [&](
        bool matches,
        std::string_view primary_name,
        std::string_view usage,
        bool& backing_value,
        auto&& apply_fn,
        auto&& save_fn,
        auto&& status_fn)
    {
        if (!matches) {
            return false;
        }

        const std::string arg_text = trim_ascii_copy(trimmed.substr(primary_name.size()));
        if (arg_text.empty()) {
            out_output_lines.push_back(status_fn());
            out_status = "Printed setting state.";
            out_success = true;
            return true;
        }

        bool requested = false;
        if (!parse_bool_like(arg_text, requested)) {
            out_output_lines.emplace_back(usage);
            out_status = "Invalid value.";
            return true;
        }

        backing_value = requested;
        apply_fn(true);
        save_fn();
        out_output_lines.push_back(status_fn());
        out_status = "Applied setting.";
        out_success = true;
        return true;
    };

    const bool is_directinput =
        starts_with_case_insensitive(trimmed, "directinput")
        || starts_with_case_insensitive(trimmed, "dinput");
    if (handle_bool_command(
            is_directinput,
            starts_with_case_insensitive(trimmed, "directinput") ? "directinput" : "dinput",
            "Usage: directinput <0|1> (alias: dinput <0|1>)",
            g_direct_input_mouse_enabled,
            apply_direct_input_mouse_mode,
            save_direct_input_mouse_setting,
            [] {
                char line[192] = {};
                std::snprintf(
                    line,
                    sizeof(line),
                    "directinput setting=%d runtime=%d",
                    g_direct_input_mouse_enabled ? 1 : 0,
                    rf2::os::input::mouse_direct_input_enabled ? 1 : 0);
                return std::string{line};
            }))
    {
        return true;
    }

    if (handle_bool_command(
            starts_with_case_insensitive(trimmed, "aimslow"),
            "aimslow",
            "Usage: aimslow <0|1>",
            g_aim_slowdown_on_target_enabled,
            apply_aim_slowdown_setting,
            save_aim_slowdown_setting,
            [] {
                char line[224] = {};
                std::snprintf(
                    line,
                    sizeof(line),
                    "aimslow setting=%d factors(min=%.3f max=%.3f)",
                    g_aim_slowdown_on_target_enabled ? 1 : 0,
                    rf2::player::autoaim::slowdown_factor_min,
                    rf2::player::autoaim::slowdown_factor_max);
                return std::string{line};
            }))
    {
        return true;
    }

    if (handle_bool_command(
            starts_with_case_insensitive(trimmed, "enemycrosshair"),
            "enemycrosshair",
            "Usage: enemycrosshair <0|1>",
            g_crosshair_enemy_indicator_enabled,
            apply_crosshair_enemy_indicator_setting,
            save_crosshair_enemy_indicator_setting,
            [] {
                char line[160] = {};
                std::snprintf(
                    line,
                    sizeof(line),
                    "enemycrosshair setting=%d",
                    g_crosshair_enemy_indicator_enabled ? 1 : 0);
                return std::string{line};
            }))
    {
        return true;
    }

    return false;
}
