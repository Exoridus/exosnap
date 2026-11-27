#include "Channel.h"
#include "Guardian.h"
#include "Peer.h"
#include "SessionIdentity.h"
#include "diagnostics/DpcLatencyProvider.h"

#include "PresentMonTraceConsumer.hpp"
#include "TraceSession.hpp"
#include <evntrace.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace exosnap::diagnostics::ipc {
namespace {
bool Elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated;
}

bool InSession(DWORD pid, DWORD session) {
    DWORD observed = 0;
    return ProcessIdToSessionId(pid, &observed) && observed == session;
}

bool Drain(HANDLE pipe, HANDLE owner, PMTraceConsumer& consumer, DWORD session) {
    std::vector<std::shared_ptr<PresentEvent>> lost;
    consumer.DequeueLostPresentEvents(lost);
    if (!lost.empty()) {
        Record record{};
        record.kind = Kind::Lost;
        record.value = lost.size();
        if (!Transfer(pipe, owner, record, true, 1000))
            return false;
    }
    std::vector<std::shared_ptr<PresentEvent>> events;
    consumer.DequeuePresentEvents(events);
    // Stop a stalled connection before allowing an unbounded transmission backlog.
    if (events.size() > 8192)
        return false;
    for (const auto& event : events) {
        if (!event || !InSession(event->ProcessId, session))
            continue;
        Record r{};
        r.kind = Kind::Present;
        r.value = event->PresentStartTime;
        r.window = event->Hwnd;
        r.process = event->ProcessId;
        r.mode = static_cast<uint8_t>(event->PresentMode);
        r.flags = (event->SupportsTearing ? 1 : 0) | (event->SyncInterval == 0 ? 2 : 0) |
                  (event->FinalState == PresentResult::Discarded ? 4 : 0);
        for (const auto& related : event->DependentPresents) {
            if (r.related_count == r.related.size())
                break;
            if (related && InSession(related->ProcessId, session))
                r.related[r.related_count++] = related->ProcessId;
        }
        if (!Transfer(pipe, owner, r, true, 1000))
            return false;
    }
    return true;
}

int Collect(const std::wstring& nonce, DWORD pid, uint64_t creation_time) {
    const auto name = PipeName(nonce);
    if (name.empty() || !pid)
        return 1;
    Handle owner(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!owner || !creation_time || CreationTime(owner.get()) != creation_time || !ApprovedClient(owner.get()))
        return 1;
    // The elevated pipe client never lends an impersonation-capable token to
    // an unelevated (and potentially compromised) local server.
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
    DWORD server = 0;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!pipe || !GetNamedPipeServerProcessId(pipe.get(), &server) || server != GetProcessId(owner.get()) ||
        !SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
        return 1;
    Record begin{};
    if (!Transfer(pipe.get(), owner.get(), begin, false, 5000) ||
        (begin.kind != Kind::Begin && begin.kind != Kind::BeginDpcOnly))
        return 1;
    auto guard = StartGuardian();
    if (!guard.guardian)
        return 1;
    if (WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT ||
        !PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, nullptr, nullptr))
        return 1;
    DpcLatencyProvider dpc;
    const bool dpc_started = dpc.StartWithIdentity(std::bit_cast<std::array<uint8_t, 16>>(DpcIdentity(guard.id)));
    PMTraceConsumer consumer;
    TraceSession trace;
    const auto status = begin.kind == Kind::Begin
                            ? trace.Start(&consumer, nullptr, nullptr, kPrivilegedSessionName, &guard.id)
                            : ERROR_NOT_SUPPORTED;
    Record ready{};
    std::atomic<bool> ended{false};
    std::thread worker;
    if (status == ERROR_SUCCESS) {
        const auto trace_handle = trace.mTraceHandle;
        worker = std::thread([&, trace_handle] {
            auto handle = trace_handle;
            ProcessTrace(&handle, 1, nullptr, nullptr);
            ended.store(true);
        });
        ready.kind = Kind::Ready;
        ready.value = static_cast<uint64_t>(trace.mTimestampFrequency.QuadPart);
    } else {
        ready.kind = Kind::Failure;
        ready.value = status == ERROR_ACCESS_DENIED    ? 2
                      : status == ERROR_ALREADY_EXISTS ? 3
                      : status == ERROR_NOT_SUPPORTED  ? 4
                                                       : 5;
    }
    DWORD session = 0;
    HANDLE lifetime[] = {owner.get(), guard.guardian.get()};
    auto next_dpc = std::chrono::steady_clock::now();
    if (ProcessIdToSessionId(pid, &session) && Transfer(pipe.get(), owner.get(), ready, true, 1000)) {
        while ((worker.joinable() || dpc_started) && WaitForMultipleObjects(2, lifetime, FALSE, 25) == WAIT_TIMEOUT) {
            SetEvent(guard.heartbeat.get());
            DWORD unexpected = 0;
            if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &unexpected, nullptr) || unexpected)
                break;
            if (worker.joinable() && ended.load()) {
                worker.join();
                trace.Stop();
                Record stopped{};
                stopped.kind = Kind::Failure;
                stopped.value = 5;
                if (!Transfer(pipe.get(), owner.get(), stopped, true, 1000))
                    break;
            }
            if (worker.joinable() && !Drain(pipe.get(), owner.get(), consumer, session))
                break;
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_dpc) {
                next_dpc = now + std::chrono::milliseconds(250);
                auto reading = dpc.Read();
                Record sample{};
                sample.kind = Kind::DpcUnavailable;
                if (reading.available && reading.metadata.Fresh(now) && reading.max_latency_us < 1.0e15) {
                    sample.kind = Kind::Dpc;
                    sample.value = static_cast<uint64_t>(reading.max_latency_us * 1000.0);
                    sample.window = std::min(sample.value, static_cast<uint64_t>(reading.avg_latency_us * 1000.0));
                    sample.process = static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - reading.metadata.observed_at)
                            .count());
                    if (reading.worst_driver.size() < sample.driver.size() &&
                        reading.worst_driver.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos)
                        std::copy(reading.worst_driver.begin(), reading.worst_driver.end(), sample.driver.begin());
                }
                if (!Transfer(pipe.get(), owner.get(), sample, true, 1000))
                    break;
            }
        }
    }
    if (worker.joinable()) {
        trace.Stop();
        worker.join();
    }
    dpc.Stop();
    return 0;
}
} // namespace
} // namespace exosnap::diagnostics::ipc

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    using namespace exosnap::diagnostics::ipc;
    if (!Elevated() || !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
        return 1;
    int count = 0;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        return 1;
    int result = 1;
    if (count == 2 && std::wstring_view(args[1]) == L"--guard") {
        result = RunGuardian();
    } else if (count == 4) {
        const std::wstring created(args[3]);
        const std::wstring pid_text(args[2]);
        if (!pid_text.empty() && pid_text.size() <= 10 && pid_text.find_first_not_of(L"0123456789") == pid_text.npos) {
            const auto pid = wcstoull(pid_text.c_str(), nullptr, 10);
            if (pid <= MAXDWORD && !created.empty() && created.size() <= 19 &&
                created.find_first_not_of(L"0123456789") == created.npos)
                result = Collect(args[1], static_cast<DWORD>(pid), wcstoull(created.c_str(), nullptr, 10));
        }
    }
    LocalFree(args);
    return result;
}
