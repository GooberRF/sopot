#pragma once

#include <windows.h>
#include <cstdint>

namespace rf2
{
    inline uintptr_t module_base()
    {
        static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return base;
    }
}
