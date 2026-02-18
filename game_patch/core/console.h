#pragma once

#include <windows.h>

void console_install_output_hook();
void console_attach_to_window(HWND window);
bool console_is_open();
