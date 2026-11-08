// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/crash_monitor.h"

#ifdef _WIN32

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
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

struct Module {
    uintptr_t base;
    uintptr_t size;
    std::string name;
};

std::vector<Module> g_modules;
HANDLE g_process = nullptr;

std::string FinalName(HANDLE file) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return {};
    }
    wchar_t buffer[1024]{};
    const DWORD n = GetFinalPathNameByHandleW(file, buffer, 1024, 0);
    if (n == 0 || n >= 1024) {
        return {};
    }
    std::wstring wide(buffer, n);
    const auto slash = wide.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        wide.erase(0, slash + 1);
    }
    char utf8[1024]{};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                        utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return std::string(utf8, len > 0 ? len : 0);
}

void AddModule(uintptr_t base, HANDLE file) {
    g_modules.push_back({base, 0, FinalName(file)});
}

/// Reads SizeOfImage from the PE header of every module, once, when a crash is reported.
void ResolveModuleSizes() {
    for (auto& m : g_modules) {
        unsigned char header[0x400]{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(m.base), header, sizeof(header),
                               &read) ||
            read < 0x100 || header[0] != 'M' || header[1] != 'Z') {
            continue;
        }
        const DWORD nt = *reinterpret_cast<DWORD*>(header + 0x3C);
        if (nt + 0x60 > sizeof(header)) {
            continue;
        }
        m.size = *reinterpret_cast<DWORD*>(header + nt + 0x18 + 0x38); // OptionalHeader.SizeOfImage
    }
}

std::string Describe(uintptr_t address) {
    for (const auto& m : g_modules) {
        if (address >= m.base && address < m.base + m.size) {
            return fmt::format("{}+{:#x}", m.name.empty() ? "?" : m.name, address - m.base);
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(g_process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != 0) {
        const char* state = mbi.State == MEM_COMMIT    ? "commit"
                            : mbi.State == MEM_RESERVE ? "reserved"
                                                       : "free";
        const char* type = mbi.Type == MEM_PRIVATE  ? "private"
                           : mbi.Type == MEM_MAPPED ? "mapped"
                           : mbi.Type == MEM_IMAGE  ? "image"
                                                    : "none";
        return fmt::format("no module (region {:#x} size {:#x} {} {} protect {:#x})",
                           reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize, state, type,
                           mbi.Protect);
    }
    return "no module";
}

bool IsExecutable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(g_process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    return mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) != 0;
}

std::string ThreadName(HANDLE thread) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!fn || thread == nullptr) {
        return {};
    }
    PWSTR wide = nullptr;
    if (FAILED(fn(thread, &wide)) || wide == nullptr) {
        return {};
    }
    char utf8[256]{};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8) - 1, nullptr,
                                        nullptr);
    LocalFree(wide);
    return std::string(utf8, len > 1 ? len - 1 : 0);
}

/// Writes a plain-text report right away: it needs no dbghelp and cannot hang.
CONTEXT ReportCrash(DWORD tid, const EXCEPTION_RECORD& record, bool& have_context) {
    ResolveModuleSizes();
    CONTEXT context{};
    context.ContextFlags = CONTEXT_ALL;
    have_context = false;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (thread != nullptr) {
        have_context = GetThreadContext(thread, &context) != FALSE;
        Note(fmt::format("  thread {} name '{}'", tid, ThreadName(thread)));
        CloseHandle(thread);
    }
    const auto address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
    Note(fmt::format("  exception address {}", Describe(address)));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const char* kind = record.ExceptionInformation[0] == 0   ? "reading"
                           : record.ExceptionInformation[0] == 1 ? "writing"
                                                                 : "executing";
        Note(fmt::format("  access violation {} {:#x}: {}", kind, record.ExceptionInformation[1],
                         Describe(record.ExceptionInformation[1])));
    }
    if (!have_context) {
        Note("  could not read the thread context");
        return context;
    }
    Note(fmt::format("  rip={:#x} rsp={:#x} rbp={:#x}", context.Rip, context.Rsp, context.Rbp));
    Note(fmt::format("  rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x}", context.Rax, context.Rbx,
                     context.Rcx, context.Rdx));
    Note(fmt::format("  rsi={:#x} rdi={:#x} r8={:#x} r9={:#x}", context.Rsi, context.Rdi,
                     context.R8, context.R9));
    Note(fmt::format("  r10={:#x} r11={:#x} r12={:#x} r13={:#x}", context.R10, context.R11,
                     context.R12, context.R13));
    Note(fmt::format("  r14={:#x} r15={:#x}", context.R14, context.R15));
    unsigned char code[16]{};
    SIZE_T read = 0;
    if (ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(context.Rip), code, sizeof(code),
                          &read) &&
        read > 0) {
        std::string bytes;
        for (SIZE_T i = 0; i < read; ++i) {
            bytes += fmt::format("{:02x} ", code[i]);
        }
        Note(fmt::format("  bytes at rip: {}", bytes));
    }
    // Raw stack scan: every stack slot that points into executable module code. Resolve against
    // the .map file of the build to read it as a call stack.
    std::vector<uint64_t> stack(0x400);
    if (ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(context.Rsp), stack.data(),
                          stack.size() * sizeof(uint64_t), &read) &&
        read >= sizeof(uint64_t)) {
        int hits = 0;
        for (size_t i = 0; i < read / sizeof(uint64_t) && hits < 60; ++i) {
            const uint64_t value = stack[i];
            for (const auto& m : g_modules) {
                if (value >= m.base && value < m.base + m.size && IsExecutable(value)) {
                    Note(fmt::format("  stack[rsp+{:#x}] {}", i * 8, Describe(value)));
                    ++hits;
                    break;
                }
            }
        }
    }
    return context;
}

struct DumpJob {
    HANDLE process;
    DWORD pid;
    DWORD tid;
    HANDLE file;
    DumpExceptionInfo* info;
};

DWORD WINAPI DumpThread(LPVOID param) {
    const auto* job = static_cast<DumpJob*>(param);
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto write_dump =
        dbghelp
            ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
            : nullptr;
    if (!write_dump) {
        return 2;
    }
    // Small and fast: stacks, thread data, module list. No data segments and no indirect memory,
    // which take minutes in a process with this much mapped memory.
    static constexpr int DumpType = 0x00000100 /* MiniDumpWithProcessThreadData */ |
                                    0x00000020 /* MiniDumpWithUnloadedModules */ |
                                    0x00001000 /* MiniDumpWithThreadInfo */;
    return write_dump(job->process, job->pid, job->file, DumpType, job->info, nullptr, nullptr)
               ? 0
               : 1;
}

void WriteDump(DWORD pid, DWORD tid, const EXCEPTION_RECORD& record, CONTEXT& context,
               bool have_context) {
    const auto path = g_dir / fmt::format("crash_monitor_{}.dmp", pid);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Note(fmt::format("could not create {}", path.string()));
        return;
    }
    EXCEPTION_RECORD exception_record = record;
    EXCEPTION_POINTERS pointers{&exception_record, &context};
    DumpExceptionInfo info{tid, &pointers, FALSE};
    DumpJob job{g_process, pid, tid, file, have_context ? &info : nullptr};
    const HANDLE thread = CreateThread(nullptr, 0, DumpThread, &job, 0, nullptr);
    DWORD result = 3;
    bool finished = false;
    if (thread != nullptr) {
        // Never wait forever: the emulator stays frozen while this runs.
        finished = WaitForSingleObject(thread, 90000) == WAIT_OBJECT_0;
        if (finished) {
            GetExitCodeThread(thread, &result);
        }
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    if (finished) {
        CloseHandle(file);
        CloseHandle(thread);
    }
    Note(finished ? fmt::format("dump {} ({} bytes): {}", path.string(), size.QuadPart,
                                result == 0 ? "written" : "FAILED")
                  : fmt::format("dump did not finish in 90 s ({} bytes), giving up",
                                size.QuadPart));
}

int Run(DWORD pid) {
    if (!DebugActiveProcess(pid)) {
        Note(fmt::format("DebugActiveProcess({}) failed, error {}", pid, GetLastError()));
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    Note(fmt::format("attached to emulator process {}", pid));

    bool first_break = true;
    bool reported = false;
    DEBUG_EVENT ev{};
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            g_process = ev.u.CreateProcessInfo.hProcess;
            AddModule(reinterpret_cast<uintptr_t>(ev.u.CreateProcessInfo.lpBaseOfImage),
                      ev.u.CreateProcessInfo.hFile);
            if (ev.u.CreateProcessInfo.hFile) {
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            AddModule(reinterpret_cast<uintptr_t>(ev.u.LoadDll.lpBaseOfDll), ev.u.LoadDll.hFile);
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
            if (ex.dwFirstChance == 0 && !reported && g_process != nullptr) {
                reported = true;
                Note(fmt::format("unhandled exception {:#x} in thread {}", code, ev.dwThreadId));
                bool have_context = false;
                CONTEXT context = ReportCrash(ev.dwThreadId, ex.ExceptionRecord, have_context);
                WriteDump(pid, ev.dwThreadId, ex.ExceptionRecord, context, have_context);
                // Do not leave the emulator frozen behind a crash.
                if (const HANDLE victim = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                    TerminateProcess(victim, code);
                    CloseHandle(victim);
                }
                Note("emulator process terminated by the monitor");
                return 0;
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
