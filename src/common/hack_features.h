// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Common {

/// Temporary per-game hack flags.
/// Initialize once with the game serial and title; query statically thereafter.
class HackFeatures {
public:
    static bool isTheOrder1886;
    static bool isNhl24;
    static bool isCodColdWar;

    /// Must be called exactly once during emulator startup, after param.sfo is parsed.
    static void Init(std::string_view game_serial, std::string_view game_title = {});

private:
    static bool initialized;
};

} // namespace Common
