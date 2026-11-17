// SPDX-FileCopyrightText: Copyright 2026 vsajjadx
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <cstddef>
#include <vector>
#include <windows.h>

namespace Common {

class WindowsProcessStartupInfo {
public:
    WindowsProcessStartupInfo() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0) {
            error = GetLastError();
            return;
        }
        storage.resize(size);
        info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &size)) {
            error = GetLastError();
            return;
        }
        initialized = true;
        if (!UpdateProcThreadAttribute(info.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy,
                                       sizeof(policy), nullptr, nullptr)) {
            error = GetLastError();
        }
    }

    ~WindowsProcessStartupInfo() {
        if (initialized) {
            DeleteProcThreadAttributeList(info.lpAttributeList);
        }
    }

    WindowsProcessStartupInfo(const WindowsProcessStartupInfo&) = delete;
    WindowsProcessStartupInfo& operator=(const WindowsProcessStartupInfo&) = delete;

    DWORD Error() const {
        return error;
    }

    STARTUPINFOW* Apply(const STARTUPINFOW& original) {
        info.StartupInfo = original;
        info.StartupInfo.cb = sizeof(info);
        return &info.StartupInfo;
    }

private:
    // Keep the host's stacks and heaps out of fixed-address guest executable ranges.
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
                     PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
    STARTUPINFOEXW info{};
    std::vector<std::byte> storage;
    DWORD error = ERROR_SUCCESS;
    bool initialized = false;
};

} // namespace Common
#endif
