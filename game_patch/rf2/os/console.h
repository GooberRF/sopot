#pragma once

#include "../rf2.h"
#include <patch_common/MemUtils.h>
#include <cstdint>

namespace rf2::os::console
{
    constexpr uintptr_t execute_command_rva = 0x0012F7D0; // sub_52F7D0
    constexpr int max_commands = 0x190;

    struct CommandEntry
    {
        const char* name;
        const char* description;
        void* callback;
        int return_type;
    };
    static_assert(sizeof(CommandEntry) == 0x10);

    static auto& command_count = addr_as_ref<int>(0x00B62F94);
    inline auto command_table_entries()
    {
        return reinterpret_cast<CommandEntry**>(0x00B62888);
    }

    inline auto execute_command_ptr()
    {
        return reinterpret_cast<int(__cdecl*)(char*)>(rf2::module_base() + execute_command_rva);
    }
}
