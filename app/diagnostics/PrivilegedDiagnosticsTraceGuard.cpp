#include "diagnostics/PrivilegedDiagnosticsTraceGuard.h"

#include <evntrace.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace exosnap::diagnostics::privileged {
namespace {

constexpr size_t kTraceCount = 2;

HANDLE ReadHandle(std::wstring_view value) {
    uintptr_t number = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9')
            return nullptr;
        const auto digit = static_cast<uintptr_t>(ch - L'0');
        if (number > (std::numeric_limits<uintptr_t>::max() - digit) / 10)
            return nullptr;
        number = number * 10 + digit;
    }
    return reinterpret_cast<HANDLE>(number);
}

bool IsExpectedOwner(HANDLE process, const ProtectedHelper& location) {
    return ProcessImageMatchesFile(process, location.executable_pin.get());
}

} // namespace

void StopOwnedTrace(TraceSlot slot, const std::wstring& nonce, const GUID& id) {
    const std::wstring name = TraceName(slot, nonce);
    struct {
        EVENT_TRACE_PROPERTIES properties{};
        std::array<wchar_t, 128> name{};
    } buffer;
    buffer.properties.Wnode.BufferSize = sizeof(buffer);
    buffer.properties.LoggerNameOffset = sizeof(buffer.properties);
    if (::ControlTraceW(0, name.c_str(), &buffer.properties, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS &&
        buffer.properties.Wnode.HistoricalContext != 0 && ::InlineIsEqualGUID(buffer.properties.Wnode.Guid, id))
        ::ControlTraceW(buffer.properties.Wnode.HistoricalContext, nullptr, &buffer.properties,
                        EVENT_TRACE_CONTROL_STOP);
}

std::wstring TraceName(TraceSlot slot, const std::wstring& nonce) {
    return (slot == TraceSlot::Present ? L"ExoSnapDiagnosticsPresent-" : L"ExoSnapDiagnosticsDpc-") + nonce;
}

bool TraceGuard::Start(const ProtectedHelper& location, const std::wstring& nonce) {
    if (::BCryptGenRandom(nullptr, reinterpret_cast<BYTE*>(ids_.data()), sizeof(ids_),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const Handle section(
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(ids_), nullptr));
    const Handle ready(::CreateEventW(&security, TRUE, FALSE, nullptr));
    HANDLE owner_handle = nullptr;
    if (!section || !ready ||
        !::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(), &owner_handle,
                           SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
        return false;
    const Handle owner(owner_handle);
    void* view = ::MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, sizeof(ids_));
    if (!view)
        return false;
    // Publish the identity before StartTrace can create the named session.
    std::memcpy(view, ids_.data(), sizeof(ids_));
    ::UnmapViewOfFile(view);

    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0)
        return false;
    std::vector<std::byte> attributes_buffer(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        return false;
    const auto attributes_release =
        std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,
                        decltype(&::DeleteProcThreadAttributeList)>(attributes, &::DeleteProcThreadAttributeList);
    HANDLE inherit[]{owner.get(), section.get(), ready.get()};
    if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit),
                                     nullptr, nullptr))
        return false;
    std::wstring command = L"\"" + location.executable + L"\" --trace-guard " + nonce + L" " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(owner.get())) + L" " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(section.get())) + L" " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(ready.get()));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(location.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                          EXTENDED_STARTUPINFO_PRESENT, nullptr, location.directory.c_str(), &startup.StartupInfo,
                          &child))
        return false;
    child_ = Handle(child.hProcess);
    const Handle child_thread(child.hThread);
    HANDLE signals[]{ready.get(), child_.get()};
    return ::WaitForMultipleObjects(2, signals, FALSE, kIoTimeoutMs) == WAIT_OBJECT_0;
}

bool TraceGuard::Alive() const {
    return child_ && ::WaitForSingleObject(child_.get(), 0) == WAIT_TIMEOUT;
}

const GUID& TraceGuard::Id(TraceSlot slot) const {
    return ids_[static_cast<size_t>(slot)];
}

bool RunTraceGuard(const ProtectedHelper& location, const std::wstring& nonce, std::wstring_view owner_arg,
                   std::wstring_view section_arg, std::wstring_view ready_arg) {
    const Handle owner(ReadHandle(owner_arg));
    const Handle section(ReadHandle(section_arg));
    const Handle ready(ReadHandle(ready_arg));
    if (!owner || !section || !ready || !IsExpectedOwner(owner.get(), location))
        return false;
    const auto* traces =
        static_cast<const GUID*>(::MapViewOfFile(section.get(), FILE_MAP_READ, 0, 0, kTraceCount * sizeof(GUID)));
    if (!traces)
        return false;
    ::SetEvent(ready.get());
    const DWORD result = ::WaitForSingleObject(owner.get(), INFINITE);
    if (result == WAIT_OBJECT_0) {
        StopOwnedTrace(TraceSlot::Present, nonce, traces[0]);
        StopOwnedTrace(TraceSlot::Dpc, nonce, traces[1]);
    }
    ::UnmapViewOfFile(traces);
    return result == WAIT_OBJECT_0;
}

} // namespace exosnap::diagnostics::privileged
