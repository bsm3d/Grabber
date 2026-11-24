// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Entry points of the user interface: the application folder, run_app and the picker test helper.

#pragma once
#include "grabber.h"

fs::path app_dir();                                            // where the settings and the presets live (%APPDATA%\BSM\Grabber)
int run_app(HINSTANCE inst, int show, const wstring& shot_path);
int picker_test(const wstring& out_file);                       // tests
