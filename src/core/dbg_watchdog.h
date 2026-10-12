// Diagnostic watchdog: periodically reports what every guest thread is doing.
#pragma once

#include <string>
#include "common/types.h"

namespace Core {
void StartDebugWatchdog();
// Describes a crash location as module+offset plus guest return addresses found on the stack.
std::string DescribeGuestCrash(u64 rip, u64 rsp);
}
