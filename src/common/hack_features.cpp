// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "common/assert.h"
#include "common/hack_features.h"

namespace Common {

bool HackFeatures::isTheOrder1886 = false;
bool HackFeatures::isNhl24 = false;
bool HackFeatures::initialized = false;

void HackFeatures::Init(std::string_view game_serial, std::string_view game_title) {
    ASSERT_MSG(!initialized, "HackFeatures::Init called more than once");
    initialized = true;

    // Classify before any title-specific early return. Never apply another game's
    // null-descriptor, resource-discovery or approximate-permutation workarounds globally.
    std::string title{game_title};
    std::ranges::transform(title, title.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    isNhl24 = game_serial == "CUSA37933" ||
              (title.find("nhl") != std::string::npos &&
               title.find("24") != std::string::npos);

    // Every regional/language release of The Order: 1886 needs these hacks. Known serials are
    // listed, and any other release is recognized by its title.
    if (game_serial == "CUSA00035" || game_serial == "CUSA00076" || game_serial == "CUSA00100" ||
        game_serial == "CUSA00785") {
        isTheOrder1886 = true;
        return;
    }
    if (title.find("the order: 1886") != std::string::npos ||
        title.find("the order 1886") != std::string::npos) {
        isTheOrder1886 = true;
    }
}

} // namespace Common
