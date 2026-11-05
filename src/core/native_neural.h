// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

// Milestone 1 of the native in-emulator neural pass (documents/native-neural.md).
// Only compiled with -DENABLE_NATIVE_NEURAL=ON on Windows; a no-op otherwise.
namespace NativeNeural {

/// Log-only probe: creates a D3D12 device on the adapter with the given LUID, starts NGX on it
/// and asks the neural runtime to initialise through the forwarder module. Nothing is rendered.
/// Returns true when the runtime's Init_Ext succeeded.
bool Probe(const std::array<uint8_t, 8>& adapter_luid);

/// Releases what Probe created.
void Shutdown();

} // namespace NativeNeural
