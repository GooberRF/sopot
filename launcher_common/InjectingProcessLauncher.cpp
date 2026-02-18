#include "InjectingProcessLauncher.h"
#include <xlog/xlog.h>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace
{

bool equals_case_insensitive(std::string_view left, std::string_view right)
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

bool compat_layer_has_run_as_invoker(std::string_view value)
{
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) {
            ++pos;
        }
        if (pos >= value.size()) {
            break;
        }

        size_t end = pos;
        while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end]))) {
            ++end;
        }

        if (equals_case_insensitive(value.substr(pos, end - pos), "RunAsInvoker")) {
            return true;
        }
        pos = end;
    }
    return false;
}

class ScopedCompatLayerRunAsInvoker
{
public:
    ScopedCompatLayerRunAsInvoker()
    {
        constexpr DWORD env_buf_cap = 32767;
        char env_buf[env_buf_cap] = {};
        DWORD env_len = GetEnvironmentVariableA("__COMPAT_LAYER", env_buf, env_buf_cap);
        if (env_len > 0 && env_len < env_buf_cap) {
            m_original.assign(env_buf, env_len);
            m_had_original = true;
        }
        else if (env_len >= env_buf_cap) {
            std::string dynamic_buf(static_cast<size_t>(env_len) + 1, '\0');
            DWORD dynamic_len =
                GetEnvironmentVariableA("__COMPAT_LAYER", dynamic_buf.data(), static_cast<DWORD>(dynamic_buf.size()));
            if (dynamic_len > 0 && dynamic_len < dynamic_buf.size()) {
                dynamic_buf.resize(dynamic_len);
                m_original = std::move(dynamic_buf);
                m_had_original = true;
            }
        }

        if (compat_layer_has_run_as_invoker(m_original)) {
            return;
        }

        std::string compat_layer = m_original;
        if (!compat_layer.empty() && !std::isspace(static_cast<unsigned char>(compat_layer.back()))) {
            compat_layer.push_back(' ');
        }
        compat_layer += "RunAsInvoker";
        if (SetEnvironmentVariableA("__COMPAT_LAYER", compat_layer.c_str())) {
            m_changed = true;
            xlog::info("Applied __COMPAT_LAYER=RunAsInvoker while creating RF2 process");
        }
    }

    ~ScopedCompatLayerRunAsInvoker()
    {
        if (!m_changed) {
            return;
        }

        if (m_had_original) {
            SetEnvironmentVariableA("__COMPAT_LAYER", m_original.c_str());
        }
        else {
            SetEnvironmentVariableA("__COMPAT_LAYER", nullptr);
        }
    }

private:
    std::string m_original;
    bool m_had_original = false;
    bool m_changed = false;
};

struct WindowSearchContext
{
    DWORD process_id = 0;
    HWND best_window = nullptr;
    LONG best_area = -1;
};

BOOL CALLBACK enum_windows_find_best_for_process_cb(HWND hwnd, LPARAM l_param)
{
    auto* context = reinterpret_cast<WindowSearchContext*>(l_param);

    DWORD window_process_id = 0;
    GetWindowThreadProcessId(hwnd, &window_process_id);
    if (window_process_id != context->process_id) {
        return TRUE;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    LONG width = std::max<LONG>(rect.right - rect.left, 0);
    LONG height = std::max<LONG>(rect.bottom - rect.top, 0);
    LONG area = width * height;
    if (area > context->best_area) {
        context->best_area = area;
        context->best_window = hwnd;
    }
    return TRUE;
}

HWND find_best_window_for_process(DWORD process_id)
{
    WindowSearchContext context{};
    context.process_id = process_id;
    EnumWindows(enum_windows_find_best_for_process_cb, reinterpret_cast<LPARAM>(&context));
    return context.best_window;
}

void force_window_foreground(HWND hwnd, DWORD process_id)
{
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    AllowSetForegroundWindow(process_id);
    ShowWindowAsync(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    DWORD foreground_tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD current_tid = GetCurrentThreadId();
    if (foreground_tid != 0 && foreground_tid != current_tid) {
        AttachThreadInput(current_tid, foreground_tid, TRUE);
    }
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (foreground_tid != 0 && foreground_tid != current_tid) {
        AttachThreadInput(current_tid, foreground_tid, FALSE);
    }
}

void attempt_bring_process_window_to_foreground(HANDLE process_handle)
{
    DWORD process_id = GetProcessId(process_handle);
    if (process_id == 0) {
        return;
    }

    // RF2 can create/minimize windows during startup; keep trying briefly.
    constexpr int attempts = 30;
    for (int i = 0; i < attempts; ++i) {
        HWND window = find_best_window_for_process(process_id);
        if (window) {
            force_window_foreground(window, process_id);
            if (!IsIconic(window) && IsWindowVisible(window)) {
                return;
            }
        }
        Sleep(100);
    }
}

} // namespace

static uintptr_t get_pe_file_entrypoint(const char* filename)
{
    std::ifstream file{filename, std::ifstream::in | std::ifstream::binary};
    if (!file)
        throw std::runtime_error(std::string("Failed to open ") + filename);
    IMAGE_DOS_HEADER dos_hdr;
    file.read(reinterpret_cast<char*>(&dos_hdr), sizeof(dos_hdr));
    if (!file || dos_hdr.e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error(std::string("Failed to read DOS header from ") + filename);

    IMAGE_NT_HEADERS32 nt_hdrs;
    file.seekg(dos_hdr.e_lfanew, std::ios_base::beg);
    file.read(reinterpret_cast<char*>(&nt_hdrs), sizeof(nt_hdrs));
    if (!file || nt_hdrs.Signature != IMAGE_NT_SIGNATURE ||
        nt_hdrs.FileHeader.SizeOfOptionalHeader < sizeof(nt_hdrs.OptionalHeader))
        throw std::runtime_error(std::string("Failed to read NT headers from ") + filename);

    return nt_hdrs.OptionalHeader.ImageBase + nt_hdrs.OptionalHeader.AddressOfEntryPoint;
}

void InjectingProcessLauncher::wait_for_process_initialization(uintptr_t entry_point, int timeout)
{
    // Change process entry point into an infinite loop (one opcode: jmp -2)
    // Based on: https://opcode0x90.wordpress.com/2011/01/15/injecting-dll-into-process-on-load/
    char buf[2];
    void* entry_point_ptr = reinterpret_cast<void*>(entry_point);
    ProcessMemoryProtection protect{m_process, entry_point_ptr, 2, PAGE_EXECUTE_READWRITE};
    m_process.read_mem(buf, entry_point_ptr, 2);
    m_process.write_mem(entry_point_ptr, "\xEB\xFE", 2);
    FlushInstructionCache(m_process.get_handle(), entry_point_ptr, 2);
    // Resume main thread
    m_thread.resume();
    // Wait untill main thread reaches the entry point
    CONTEXT context;
    DWORD start_ticks = GetTickCount();
    do {
        Sleep(50);
        context.ContextFlags = CONTEXT_CONTROL;
        try {
            m_thread.get_context(&context);
        }
        catch (const Win32Error&) {
            if (m_thread.get_exit_code() != STILL_ACTIVE)
                throw ProcessTerminatedError();
            throw;
        }

        if (context.Eip == entry_point)
            break;
    } while (static_cast<int>(GetTickCount() - start_ticks) < timeout);
    if (context.Eip != entry_point)
        THROW_EXCEPTION("timeout");
    // Suspend main thread
    m_thread.suspend();
    // Revert changes to entry point
    m_process.write_mem(entry_point_ptr, buf, 2);
    FlushInstructionCache(m_process.get_handle(), entry_point_ptr, 2);
}

InjectingProcessLauncher::InjectingProcessLauncher(
    const char* app_name, const char* work_dir, const char* command_line, STARTUPINFO& startup_info, int timeout)
{
    xlog::info("Creating suspended process");
    PROCESS_INFORMATION process_info;
    ZeroMemory(&process_info, sizeof(process_info));
    ScopedCompatLayerRunAsInvoker compat_layer_override{};
    xlog::info("Calling CreateProcessA app_name {}, command_line {}, work_dir {}", app_name, command_line, work_dir);
    BOOL result = CreateProcessA(app_name, const_cast<char*>(command_line), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                                 nullptr, work_dir, &startup_info, &process_info);
    xlog::info("CreateProcessA returned {}", result);
    if (!result) {
        THROW_WIN32_ERROR();
    }

    m_process = Process{process_info.hProcess};
    m_thread = Thread{process_info.hThread};

    xlog::info("Finding entry-point");
    uintptr_t entry_point = get_pe_file_entrypoint(app_name);
    xlog::info("Waiting for process initialization");
    wait_for_process_initialization(entry_point, timeout);
}

void InjectingProcessLauncher::resume_main_thread()
{
    m_thread.resume();
    m_resumed = true;
    attempt_bring_process_window_to_foreground(m_process.get_handle());
}
