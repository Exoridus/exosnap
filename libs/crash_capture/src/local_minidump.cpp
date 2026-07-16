// local_minidump.cpp — In-process minidump fallback (see local_minidump.h).
//
// Everything that runs inside the exception filter must be allocation-free and
// must not touch the CRT locale/heap: the process is already dying and its heap
// may be the reason. The crash directory is therefore snapshotted into a fixed
// buffer at install time, and the dump path is formatted with swprintf_s into
// stack storage.

#include <crash_capture/local_minidump.h>

#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dbghelp.h must follow windows.h.
#include <dbghelp.h>

namespace exosnap::crash_capture {
namespace {

// Room for the crash directory plus "\exosnap-YYYYMMDD-HHMMSS-<pid>.dmp".
constexpr size_t kDumpNameMax = 64;
constexpr size_t kCrashDirMax = 512;
constexpr size_t kDumpPathMax = kCrashDirMax + kDumpNameMax;

// Snapshotted at install time; read from the exception filter. Written once
// before any worker thread exists, so no synchronisation is needed.
wchar_t g_crash_dir[kCrashDirMax] = {};

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
bool g_installed = false;

// Serializes MiniDumpWriteDump calls: Microsoft documents dbghelp.dll as not
// safe to call concurrently from multiple threads of the same process, so two
// threads crashing close together could otherwise race inside it — a hang or a
// corrupt dump. A raw CRITICAL_SECTION (not std::mutex) so entering/leaving it
// from the exception filter touches only kernel32, never the CRT/heap (see the
// file banner above). Initialized once, outside crash context, alongside the
// filter install below.
CRITICAL_SECTION g_dump_lock;

// What goes into the dump. Deliberately not MiniDumpWithFullMemory: ExoSnap
// holds large video staging buffers, and a full dump of a recording session runs
// to gigabytes. This set keeps every thread's stack, the referenced heap blocks
// around them, handles, and unloaded modules — enough to resolve a dangling
// pointer in teardown, which is the class of crash this exists to catch.
constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData);

LONG WINAPI WriteDumpFilter(EXCEPTION_POINTERS* exception_pointers) {
    if (g_crash_dir[0] == L'\0')
        return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t path[kDumpPathMax] = {};
    const int written = swprintf_s(
        path, L"%s\\exosnap-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_crash_dir, static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond), GetCurrentProcessId());
    if (written <= 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception_pointers;
    info.ClientPointers = FALSE;

    EnterCriticalSection(&g_dump_lock);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, exception_pointers ? &info : nullptr,
                      nullptr, nullptr);
    LeaveCriticalSection(&g_dump_lock);
    CloseHandle(file);

    // Hand control back to any previously installed filter (Windows Error
    // Reporting included) so behaviour beyond the dump is unchanged.
    if (g_previous_filter != nullptr)
        return g_previous_filter(exception_pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool ShouldInstallLocalMinidumpHandler(bool crashpad_active) noexcept {
    return !crashpad_active;
}

std::string MakeMinidumpFileName(int year, int month, int day, int hour, int minute, int second, unsigned long pid) {
    char buf[kDumpNameMax] = {};
    std::snprintf(buf, sizeof(buf), "exosnap-%04d%02d%02d-%02d%02d%02d-%lu.dmp", year, month, day, hour, minute, second,
                  pid);
    return std::string(buf);
}

bool InstallLocalMinidumpHandler(const std::string& crash_dir) {
    if (crash_dir.empty())
        return false;

    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, crash_dir.c_str(), -1, nullptr, 0);
    if (wide_len <= 0 || static_cast<size_t>(wide_len) > kCrashDirMax)
        return false;
    if (MultiByteToWideChar(CP_UTF8, 0, crash_dir.c_str(), -1, g_crash_dir, wide_len) <= 0) {
        g_crash_dir[0] = L'\0';
        return false;
    }

    if (!g_installed) {
        InitializeCriticalSection(&g_dump_lock);
        g_previous_filter = SetUnhandledExceptionFilter(WriteDumpFilter);
        g_installed = true;
    }
    return true;
}

} // namespace exosnap::crash_capture
