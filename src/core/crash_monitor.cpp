// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/crash_monitor.h"

#ifdef _WIN32

#include <cstdlib>
#include <fstream>
#include <string>
#include <fmt/format.h>
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace Core::CrashMonitor {

namespace {

std::filesystem::path g_dir;

void Note(const std::string& text) {
    std::ofstream file(g_dir / "crash_monitor.txt", std::ios::app);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    file << fmt::format("{:02}:{:02}:{:02}.{:03} {}\n", t.wHour, t.wMinute, t.wSecond,
                        t.wMilliseconds, text);
}

// MINIDUMP_EXCEPTION_INFORMATION is declared with pack(4) in dbghelp.h.
#pragma pack(push, 4)
struct DumpExceptionInfo {
    DWORD thread_id;
    EXCEPTION_POINTERS* pointers;
    BOOL client_pointers;
};
#pragma pack(pop)

void WriteDump(HANDLE process, DWORD pid, DWORD tid, const EXCEPTION_RECORD& record) {
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto write_dump =
        dbghelp
            ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
            : nullptr;
    if (!write_dump) {
        Note("dbghelp.dll is not available, no dump written");
        return;
    }
    const auto path = g_dir / fmt::format("crash_monitor_{}.dmp", pid);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Note(fmt::format("could not create {}", path.string()));
        return;
    }
    // The context of the faulting thread, stopped at the exception. The structures live in this
    // process, so ClientPointers is FALSE.
    CONTEXT context{};
    context.ContextFlags = CONTEXT_ALL;
    EXCEPTION_RECORD exception_record = record;
    EXCEPTION_POINTERS pointers{&exception_record, &context};
    DumpExceptionInfo info{tid, &pointers, FALSE};
    bool have_context = false;
    if (const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid)) {
        have_context = GetThreadContext(thread, &context) != FALSE;
        CloseHandle(thread);
    }
    static constexpr int DumpType = 0x00000001 /* MiniDumpWithDataSegs */ |
                                    0x00000040 /* MiniDumpWithIndirectlyReferencedMemory */ |
                                    0x00000020 /* MiniDumpWithUnloadedModules */ |
                                    0x00001000 /* MiniDumpWithThreadInfo */ |
                                    0x00000800 /* MiniDumpWithFullMemoryInfo */;
    const BOOL ok = write_dump(process, pid, file, DumpType, have_context ? &info : nullptr,
                               nullptr, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(file);
    Note(ok ? fmt::format("dump written to {}", path.string())
            : fmt::format("writing dump failed, error {:#x}", error));
}

int Run(DWORD pid) {
    if (!DebugActiveProcess(pid)) {
        Note(fmt::format("DebugActiveProcess({}) failed, error {}", pid, GetLastError()));
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    Note(fmt::format("attached to emulator process {}", pid));

    HANDLE process = nullptr;
    bool first_break = true;
    bool dumped = false;
    DEBUG_EVENT ev{};
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            process = ev.u.CreateProcessInfo.hProcess;
            if (ev.u.CreateProcessInfo.hFile) {
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) {
                CloseHandle(ev.u.LoadDll.hFile);
            }
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& ex = ev.u.Exception;
            const DWORD code = ex.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT && first_break) {
                // The break-in of the attach itself, not an exception of the emulator.
                first_break = false;
                break;
            }
            // Everything else goes to the emulator's own handlers first.
            status = DBG_EXCEPTION_NOT_HANDLED;
            if (ex.dwFirstChance == 0 && !dumped && process != nullptr) {
                dumped = true;
                Note(fmt::format("unhandled exception {:#x} at {:#x} in thread {}", code,
                                 reinterpret_cast<uintptr_t>(ex.ExceptionRecord.ExceptionAddress),
                                 ev.dwThreadId));
                WriteDump(process, pid, ev.dwThreadId, ex.ExceptionRecord);
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            Note(fmt::format("emulator process exited, exit code {:#x}",
                             ev.u.ExitProcess.dwExitCode));
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return 0;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    Note(fmt::format("WaitForDebugEvent failed, error {}", GetLastError()));
    return 1;
}

} // namespace

bool TryRun(int& exit_code) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }
    bool is_monitor = false;
    if (argc >= 4 && std::wstring(argv[1]) == L"--crash-monitor") {
        is_monitor = true;
        g_dir = std::filesystem::path(argv[3]);
        exit_code = Run(static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)));
    }
    LocalFree(argv);
    return is_monitor;
}

void Spawn(const std::filesystem::path& dir) {
    if (GetEnvironmentVariableW(L"SHADPS4_NO_CRASH_MONITOR", nullptr, 0) != 0 ||
        IsDebuggerPresent()) {
        return;
    }
    std::wstring exe(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length == 0 || length >= exe.size()) {
        return;
    }
    exe.resize(length);
    std::wstring dir_text = dir.wstring();
    while (!dir_text.empty() && (dir_text.back() == L'\\' || dir_text.back() == L'/')) {
        dir_text.pop_back();
    }
    std::wstring command = L"\"" + exe + L"\" --crash-monitor " +
                           std::to_wstring(GetCurrentProcessId()) + L" \"" + dir_text + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                       &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
}

} // namespace Core::CrashMonitor

#else

namespace Core::CrashMonitor {
bool TryRun(int&) {
    return false;
}
void Spawn(const std::filesystem::path&) {}
} // namespace Core::CrashMonitor

#endif
