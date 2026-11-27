#include "Guardian.h"
#include "SessionIdentity.h"
#include "StartingGuardian.h"
#include "diagnostics/DpcLatencyProvider.h"

#include <evntrace.h>
#include <objbase.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::diagnostics::ipc {
namespace {
struct TraceQuery {
    EVENT_TRACE_PROPERTIES properties{};
    std::array<wchar_t, 256> logger{};
};

} // namespace

std::wstring GlobalProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    auto size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, PROCESS_NAME_NATIVE, path.data(), &size))
        return {};
    path.resize(size);
    constexpr std::wstring_view prefix = LR"(\Device\HarddiskVolume)";
    if (!path.starts_with(prefix))
        return {};
    const auto separator = path.find_first_not_of(L"0123456789", prefix.size());
    if (separator == prefix.size() || separator == std::wstring::npos || path[separator] != L'\\' ||
        separator + 1 == path.size())
        return {};
    return LR"(\\?\GLOBALROOT)" + path;
}

void StopOwnedSession(const wchar_t* name, const GUID& id) {
    TraceQuery query{};
    query.properties.Wnode.BufferSize = sizeof(query);
    query.properties.LoggerNameOffset = offsetof(TraceQuery, logger);
    if (ControlTraceW(0, name, &query.properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_SUCCESS ||
        !IsEqualGUID(query.properties.Wnode.Guid, id))
        return;
    const auto handle = query.properties.Wnode.HistoricalContext;
    if (handle)
        ControlTraceW(handle, nullptr, &query.properties, EVENT_TRACE_CONTROL_STOP);
}

int RunGuardian() {
    Handle owner(GetStdHandle(STD_INPUT_HANDLE));
    Handle identity_pipe(GetStdHandle(STD_OUTPUT_HANDLE));
    Handle heartbeat(GetStdHandle(STD_ERROR_HANDLE));
    if (!owner || !identity_pipe || !heartbeat || GetFileType(identity_pipe.get()) != FILE_TYPE_PIPE)
        return 1;
    const auto own_path = GlobalProcessImagePath(GetCurrentProcess());
    if (own_path.empty() || own_path != GlobalProcessImagePath(owner.get()) ||
        GetProcessId(owner.get()) == GetCurrentProcessId())
        return 1;
    GUID id{};
    DWORD written = 0;
    if (FAILED(CoCreateGuid(&id)) || !WriteFile(identity_pipe.get(), &id, sizeof(id), &written, nullptr) ||
        written != sizeof(id))
        return 1;
    identity_pipe.reset();
    HANDLE waits[] = {owner.get(), heartbeat.get()};
    DWORD result = WAIT_FAILED;
    do {
        result = WaitForMultipleObjects(2, waits, FALSE, 20000);
    } while (result == WAIT_OBJECT_0 + 1);
    if (result == WAIT_TIMEOUT) {
        // The collector can stall inside a native stop/join as well as crash.
        // Only the inherited process object for that exact image is terminable.
        if (!TerminateProcess(owner.get(), ERROR_TIMEOUT) || WaitForSingleObject(owner.get(), 5000) != WAIT_OBJECT_0)
            return 1;
    } else if (result != WAIT_OBJECT_0) {
        return 1;
    }
    StopOwnedSession(kPrivilegedSessionName, id);
    StopOwnedSession(kDpcSessionName, DpcIdentity(id));
    return 0;
}

GuardedSession StartGuardian() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE reader_raw = nullptr;
    HANDLE writer_raw = nullptr;
    if (!CreatePipe(&reader_raw, &writer_raw, &security, 0))
        return {};
    Handle reader(reader_raw);
    Handle writer(writer_raw);
    Handle owner(
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, TRUE, GetCurrentProcessId()));
    Handle heartbeat(CreateEventW(&security, FALSE, FALSE, nullptr));
    if (!owner || !heartbeat)
        return {};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        return {};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = owner.get();
    startup.StartupInfo.hStdOutput = writer.get();
    startup.StartupInfo.hStdError = heartbeat.get();
    startup.lpAttributeList = list;
    HANDLE inherited[] = {owner.get(), writer.get(), heartbeat.get()};
    const auto path = GlobalProcessImagePath(GetCurrentProcess());
    auto args = L"\"" + path + L"\" --guard";
    PROCESS_INFORMATION child{};
    const bool launched =
        !path.empty() &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr,
                                  nullptr) &&
        CreateProcessW(path.c_str(), args.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(list);
    if (!launched)
        return {};
    StartingGuardian process{Handle(child.hProcess)};
    Handle thread(child.hThread);
    writer.reset();
    // The writer is an inherited allowlisted pipe held only by the pinned child.
    // Polling bounds initialization even if image startup stalls before main.
    for (unsigned attempt = 0; attempt != 100; ++attempt) {
        DWORD available = 0;
        if (!PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr))
            return {};
        if (available >= sizeof(GUID)) {
            GUID id{};
            DWORD received = 0;
            if (ReadFile(reader.get(), &id, sizeof(id), &received, nullptr) && received == sizeof(id))
                return {process.handoff(), id, std::move(heartbeat)};
            return {};
        }
        if (WaitForSingleObject(process.get(), 50) != WAIT_TIMEOUT)
            break;
    }
    return {};
}
} // namespace exosnap::diagnostics::ipc
