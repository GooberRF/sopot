#pragma once

#include <windows.h>

void console_install_output_hook();
void console_attach_to_window(HWND window);
bool console_is_open();
void console_on_present(HWND target_window);
