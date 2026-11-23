// Grabber: entry points of the user interface.
#pragma once
#include "grabber.h"

fs::path app_dir();                                            // where the settings and the presets live (%APPDATA%\BSM\Grabber)
int run_app(HINSTANCE inst, int show, const wstring& shot_path);
int picker_test(const wstring& out_file);                       // tests
