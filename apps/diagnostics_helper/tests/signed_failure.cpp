#include "signed_failure.h"

#include "../FixtureStatus.h"
#include "../Guardian.h"
#include "../ImagePath.h"
#include "../Peer.h"
#include "../StartingGuardian.h"
#include "diagnostics/PrivilegedPresentBackend.h"

#include <evntrace.h>
#include <tlhelp32.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace exosnap::diagnostics::fixture {
namespace {
using namespace ipc;
using namespace std::chrono_literals;

struct ProcessId {
    DWORD pid = 0;
    uint64_t created = 0;
};

struct Receipt {
    ProcessId collector;
    ProcessId guardian;
    GUID session{};
};

struct Pair {
    Handle collector;
    Handle guardian;
    GUID session{};
};

template <class Predicate> bool Until(Predicate predicate, std::chrono::seconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(50ms);
    }
    return true;
}

ProcessId Identity(HANDLE process) {
    return {GetProcessId(process), CreationTime(process)};
}

Handle OpenIdentity(ProcessId id) {
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id.pid));
    if (!process || !id.created || CreationTime(process.get()) != id.created ||
        WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        return {};
    return process;
}

Handle GuardianOf(HANDLE collector) {
    const auto owner = Identity(collector);
    const auto image = NativeProcessImagePath(collector);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (image.empty() || !snapshot || !Process32FirstW(snapshot.get(), &entry))
        return {};
    Handle result;
    do {
        if (entry.th32ParentProcessID != owner.pid || _wcsicmp(entry.szExeFile, L"exosnap-diagnostics.exe") != 0)
            continue;
        Handle candidate(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!candidate || NativeProcessImagePath(candidate.get()) != image ||
            CreationTime(candidate.get()) < owner.created || WaitForSingleObject(candidate.get(), 0) != WAIT_TIMEOUT)
            continue;
        if (result)
            return {};
        result = std::move(candidate);
    } while (Process32NextW(snapshot.get(), &entry));
    return result;
}

bool TerminateExact(HANDLE observed) {
    if (!observed || WaitForSingleObject(observed, 0) != WAIT_TIMEOUT)
        return false;
    Handle target(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                              GetProcessId(observed)));
    return target && CompareObjectHandles(target.get(), observed) && TerminateProcess(target.get(), 217);
}

bool Retired(Pair& pair) {
    const HANDLE handles[] = {pair.collector.get(), pair.guardian.get()};
    const auto wait = WaitForMultipleObjects(2, handles, TRUE, 30000);
    const bool absent = Until([] { return QueryDpcSession() == ERROR_WMI_INSTANCE_NOT_FOUND; }, 5s);
    std::cout << "Owned collector/guardian wait=" << wait << ", DPC absent=" << absent << '\n';
    if (wait == WAIT_OBJECT_0 && absent)
        return true;
    // The failure remains red. Only the two identified processes and exact
    // observed generation may be retired during disposable-machine teardown.
    TerminateExact(handles[0]);
    WaitForSingleObject(handles[0], 2000);
    TerminateExact(handles[1]);
    WaitForSingleObject(handles[1], 2000);
    StopOwnedSession(kDpcSessionName, pair.session);
    return false;
}

class Active {
public:
    bool Start(Pair& result) {
        if (QueryDpcSession() != ERROR_WMI_INSTANCE_NOT_FOUND || backend_->Open() != PresentTraceOpenResult::Opened)
            return false;
        worker_ = std::thread([this] { backend_->Consume(); });
        if (!Until([this] { return backend_->HelperState() != DiagnosticsHelperState::Starting; }, 90s) ||
            backend_->HelperState() != DiagnosticsHelperState::Connected ||
            QueryDpcSession(&result.session) != ERROR_SUCCESS || IsEqualGUID(result.session, GUID{}))
            return false;
        result.collector = TakeFixtureCollector();
        if (!result.collector)
            return false;
        result.guardian = GuardianOf(result.collector.get());
        return static_cast<bool>(result.guardian);
    }
    bool Failed() const {
        return Until([this] { return backend_->HelperState() == DiagnosticsHelperState::Failed; }, 10s);
    }
    ~Active() {
        backend_->Close();
        if (worker_.joinable())
            worker_.join();
    }
private:
    std::shared_ptr<IPrivilegedDiagnosticsBackend> backend_ = MakePrivilegedDiagnosticsBackend(false);
    std::thread worker_;
};

bool ForceMember(bool guardian) {
    Active active;
    Pair pair;
    if (!active.Start(pair) || !TerminateExact(guardian ? pair.guardian.get() : pair.collector.get()))
        return false;
    const bool retired = Retired(pair);
    const bool failed = active.Failed();
    std::cout << (guardian ? "Guardian" : "Collector") << " forced exit, unsolicited disconnect reported failure=" << failed << '\n';
    return retired && failed;
}

bool ForceRecorder() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE raw_read = nullptr, raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &security, 0))
        return false;
    Handle reader(raw_read), writer(raw_write);
    if (!SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0))
        return false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> buffer(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
        return false;
    auto inherited = writer.get();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    const auto image = GlobalLaunchPath(GlobalProcessImagePath(GetCurrentProcess()));
    auto command = L"\"" + image + L"\" --hold-owned-cycle " + std::to_wstring(HandleToULong(inherited));
    const bool launched = !image.empty() &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof(inherited), nullptr, nullptr) &&
        CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                       nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(list);
    if (!launched)
        return false;
    StartingGuardian child{Handle(process.hProcess)};
    Handle main_thread(process.hThread);
    writer.reset();
    DWORD available = 0;
    if (!Until([&] {
            return !PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr) ||
                   available >= sizeof(Receipt) || WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT;
        }, 100s) || available < sizeof(Receipt))
        return false;
    Receipt receipt{};
    DWORD read = 0;
    if (!ReadFile(reader.get(), &receipt, sizeof(receipt), &read, nullptr) || read != sizeof(receipt))
        return false;
    Pair pair{OpenIdentity(receipt.collector), OpenIdentity(receipt.guardian), receipt.session};
    GUID current{};
    if (!pair.collector || !pair.guardian || QueryDpcSession(&current) != ERROR_SUCCESS ||
        !IsEqualGUID(current, pair.session))
        return false;
    auto observed_guardian = GuardianOf(pair.collector.get());
    if (!observed_guardian || !CompareObjectHandles(observed_guardian.get(), pair.guardian.get()) ||
        !TerminateProcess(child.get(), 218) || WaitForSingleObject(child.get(), 5000) != WAIT_OBJECT_0)
        return false;
    std::cout << "Exact recorder child terminated after authenticated Ready\n";
    return Retired(pair);
}
} // namespace

ULONG QueryDpcSession(GUID* id) {
    struct Query {
        EVENT_TRACE_PROPERTIES properties{};
        std::array<wchar_t, 256> name{};
    } query;
    query.properties.Wnode.BufferSize = sizeof(query);
    query.properties.LoggerNameOffset = offsetof(Query, name);
    const auto status = ControlTraceW(0, kDpcSessionName, &query.properties, EVENT_TRACE_CONTROL_QUERY);
    if (status == ERROR_SUCCESS && id)
        *id = query.properties.Wnode.Guid;
    return status;
}

bool HoldOwnedCycle(std::string_view argument) {
    ULONG value = 0;
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (!value || parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size())
        return false;
    Handle writer(ULongToHandle(value));
    if (GetFileType(writer.get()) != FILE_TYPE_PIPE)
        return false;
    Active active;
    Pair pair;
    if (!active.Start(pair))
        return false;
    const Receipt receipt{Identity(pair.collector.get()), Identity(pair.guardian.get()), pair.session};
    DWORD sent = 0;
    if (!WriteFile(writer.get(), &receipt, sizeof(receipt), &sent, nullptr) || sent != sizeof(receipt))
        return false;
    writer.reset();
    // The supervisor terminates this exact process. A lost supervisor still
    // releases the fixture's signed backend and its owned session by this deadline.
    std::this_thread::sleep_for(45s);
    return false;
}

bool RunForcedExitCases() {
    return ForceRecorder() && ForceMember(false) && ForceMember(true);
}
} // namespace exosnap::diagnostics::fixture
