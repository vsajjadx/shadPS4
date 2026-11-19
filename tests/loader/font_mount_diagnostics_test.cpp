// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "core/font_mount_diagnostics.h"

int main() {
    using Core::FontMountState;
    using Core::InspectFontMount;
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() /
                ("shad-font-mount-" + std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(path)) {
        return 1;
    }
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{path};
    const auto font = path / "font";
    const auto font2 = path / "font2";
    fs::create_directory(font);
    fs::create_directory(font2);
    std::ofstream(font / "fixture.txt") << "directory content, not an actual font";
    const auto check = [](FontMountState actual, FontMountState expected, const char* scenario) {
        if (actual != expected) {
            std::cerr << scenario << '\n';
            return false;
        }
        return true;
    };
    return check(InspectFontMount(font), FontMountState::Populated, "first mount populated") &&
                   check(InspectFontMount(font2), FontMountState::Empty, "second mount empty") &&
                   check(InspectFontMount(path / "missing"), FontMountState::Unavailable,
                         "missing mount") &&
                   check(InspectFontMount(font / "fixture.txt"), FontMountState::Unavailable,
                         "not a directory")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
