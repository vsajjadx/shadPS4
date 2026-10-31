// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Plays pad speaker audio ports on the speaker of a DualSense, over USB (its 4-channel audio
// endpoint) or Bluetooth (Opus in HID output report 0x31/0x39). Ported from KytyPS5
// (src/libs/dualSenseHaptics.cpp, src/libs/dualSenseBluetooth.cpp, MIT).
namespace Libraries::AudioOut::DualSenseSpeaker {

struct Stream;

Stream* Open(u32 freq);
void Close(Stream* stream);

/// Queues interleaved float samples. Returns how long the queued audio still plays in
/// microseconds, or 0 when no DualSense took it (the caller then plays it as usual).
u64 Queue(Stream* stream, const float* data, u32 frames, u32 channels, float gain);

/// Restores the headphone routing and stops the Bluetooth sender.
void Shutdown();

} // namespace Libraries::AudioOut::DualSenseSpeaker
