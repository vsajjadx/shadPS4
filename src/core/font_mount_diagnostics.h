// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace Core {

enum class FontMountState { Empty, Populated, Unavailable };

// This is a directory inspection, not validation of console font files.
inline FontMountState InspectFontMount(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
        return FontMountState::Unavailable;
    }
    const bool empty = std::filesystem::is_empty(path, ec);
    if (ec) {
        return FontMountState::Unavailable;
    }
    return empty ? FontMountState::Empty : FontMountState::Populated;
}

} // namespace Core
