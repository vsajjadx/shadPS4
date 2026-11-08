// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace Core::CrashMonitor {

/// Windows only. If this process was started as a crash monitor (--crash-monitor <pid> <dir>),
/// runs the monitor, stores its exit code and returns true. Returns false for a normal start.
bool TryRun(int& exit_code);

/// Windows only. Starts a monitor child that attaches to this process as a debugger. It writes
/// crash_monitor_<pid>.dmp for any unhandled exception, including fast fail and heap corruption
/// that never reach the emulator's own handlers, and records the exit code in crash_monitor.txt.
/// Set SHADPS4_NO_CRASH_MONITOR=1 to disable it.
void Spawn(const std::filesystem::path& dir);

} // namespace Core::CrashMonitor
