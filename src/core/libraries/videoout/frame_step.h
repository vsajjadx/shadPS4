// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::VideoOut {

/// Diagnostic frame stepping, applied by the present thread. While frozen no flip is taken and
/// the last frame stays on screen; in slow motion one flip is taken per second.
/// Both return the new state.
bool ToggleFrameFreeze();
bool ToggleSlowMotion();

bool IsFrameFrozen();
bool IsSlowMotion();

} // namespace Libraries::VideoOut
