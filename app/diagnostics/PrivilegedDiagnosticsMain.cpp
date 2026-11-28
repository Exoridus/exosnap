#include "diagnostics/DpcLatencyProvider.h"
#include "diagnostics/PresentTraceBackend.h"
#include "diagnostics/PrivilegedDiagnosticsProtocol.h"
#include "diagnostics/PrivilegedDiagnosticsTraceGuard.h"

#include <shellapi.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace exosnap::diagnostics::privileged {
namespace {

bool IsElevated() {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    const Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return ::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes) &&
           elevation.TokenIsElevated != 0;
}

uint64_t PositiveDecimal(std::wstring_view input) {
    if (input.empty() || input.size() > 19 || input.find_first_not_of(L"0123456789") != std::wstring_view::npos)
        return 0;
    uint64_t value = 0;
    for (const wchar_t ch : input)
        value = value * 10 + static_cast<uint64_t>(ch - L'0');
    return value;
}

class PresentationTrace {
  public:
    ~PresentationTrace() {
        if (!backend_)
            return;
        backend_->Close();
        if (worker_.joinable()) {
            if (::WaitForSingleObject(worker_.native_handle(), 2000) == WAIT_OBJECT_0)
                worker_.join();
            else
                worker_.detach();
        }
    }

    void Start(const std::wstring& nonce, const TraceGuard& guard) {
        backend_ = MakeNamedPresentTraceBackend(TraceName(TraceSlot::Present, nonce), guard.Id(TraceSlot::Present));
        if (!backend_ || backend_->Open() != PresentTraceOpenResult::Opened) {
            backend_.reset();
            return;
        }
        worker_ = std::thread([backend = backend_]() { backend->Consume(); });
    }

    void Read(Response& response, DWORD owner_session) {
        if (!backend_ || ::WaitForSingleObject(worker_.native_handle(), 0) != WAIT_TIMEOUT) {
            response.present_state = PresentStatus::Unavailable;
            return;
        }
        response.frequency = backend_->TimestampFrequency();
        if (response.frequency <= 0) {
            response.frequency = 0;
            response.present_state = PresentStatus::Unavailable;
            return;
        }
        response.present_state = PresentStatus::Open;
        response.lost = backend_->DrainLostPresentCount().value_or(0);
        const auto drained = backend_->Drain();
        std::unordered_map<DWORD, bool> in_session;
        const auto owns = [owner_session, &in_session](DWORD pid) {
            const auto [entry, inserted] = in_session.try_emplace(pid, false);
            if (inserted) {
                DWORD session = 0;
                entry->second = ::ProcessIdToSessionId(pid, &session) && session == owner_session;
            }
            return entry->second;
        };
        for (const auto& event : drained) {
            if (event.present_qpc == 0 || !owns(event.process_id))
                continue;
            if (response.present_count == kMaxPresents) {
                // A transport limit is not a count of incomplete ETW presents.
                response = {};
                response.present_state = PresentStatus::Unavailable;
                return;
            }
            auto& row = response.presents[response.present_count++];
            row.pid = event.process_id;
            row.hwnd = event.hwnd;
            row.qpc = event.present_qpc;
            row.mode = event.present_mode_code;
            row.sync_interval = event.sync_interval;
            row.flags = (event.tearing_flag ? 1U : 0U) | (event.discarded ? 2U : 0U);
            for (const DWORD pid : event.related_process_ids) {
                if (!owns(pid))
                    continue;
                if (row.related_count == kMaxRelated) {
                    response = {};
                    response.present_state = PresentStatus::Unavailable;
                    return;
                }
                row.related[row.related_count++] = pid;
            }
        }
    }

  private:
    std::shared_ptr<IPresentTraceBackend> backend_;
    std::thread worker_;
};

void ReadDpc(const DpcLatencyProvider& provider, Response& response) {
    if (!provider.IsOpen())
        return;
    response.dpc_open = 1;
    const DpcLatencyReading sample = provider.Read();
    const auto now = std::chrono::steady_clock::now();
    const int64_t frequency = QpcFrequency();
    const uint64_t qpc = QpcNow();
    if (!sample.available || !sample.metadata.Fresh(now) || frequency <= 0)
        return;
    const auto age = static_cast<uint64_t>(std::chrono::duration<double>(now - sample.metadata.observed_at).count() *
                                           static_cast<double>(frequency));
    if (qpc <= age)
        return;
    response.dpc_qpc = qpc - age;
    response.dpc_max_us = sample.max_latency_us;
    response.dpc_average_us = std::min(sample.avg_latency_us, sample.max_latency_us);
    if (sample.worst_driver.find_first_of("/\\:") == std::string::npos)
        std::copy_n(sample.worst_driver.begin(), std::min(sample.worst_driver.size(), response.dpc_driver.size() - 1),
                    response.dpc_driver.begin());
}

bool Collect(const ProtectedHelper& location, const std::wstring& pipe_nonce, DWORD owner_pid, uint64_t owner_birth) {
    DWORD session = 0;
    if (owner_pid == 0 || owner_birth == 0 || !SameSession(owner_pid) || !::ProcessIdToSessionId(owner_pid, &session))
        return false;
    const Handle owner(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner_pid));
    if (!owner || ProcessBirth(owner.get()) != owner_birth || !IsPairedRecorder(location, owner.get()))
        return false;
    const std::wstring name = kPipePrefix + pipe_nonce;
    const Handle pipe = OpenPipeClient(name);
    ULONG server_pid = 0;
    if (!pipe || !::GetNamedPipeServerProcessId(pipe.get(), &server_pid) || server_pid != owner_pid ||
        ::WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT)
        return false;
    Request request{};
    if (!Transfer(pipe.get(), request, false, owner.get()) || !IsValid(request))
        return false;

    // Session names come from a privileged RNG, not from the pipe peer.
    const auto trace_nonce = NewNonce();
    TraceGuard cleanup;
    if (trace_nonce.empty() || !cleanup.Start(location, trace_nonce))
        return false;
    bool wants_present = request.present != 0;
    std::optional<PresentationTrace> present;
    if (wants_present)
        present.emplace().Start(trace_nonce, cleanup);
    DpcLatencyProvider dpc;
    (void)dpc.Start(TraceName(TraceSlot::Dpc, trace_nonce), cleanup.Id(TraceSlot::Dpc));

    do {
        if (!cleanup.Alive() || (request.present != 0 && !wants_present))
            break;
        if (request.present == 0 && wants_present) {
            present.reset();
            wants_present = false;
        }
        Response response{};
        if (present) {
            present->Read(response, session);
            if (response.present_state == PresentStatus::Unavailable)
                present.reset();
        } else if (wants_present) {
            response.present_state = PresentStatus::Unavailable;
        }
        ReadDpc(dpc, response);
        if (!Transfer(pipe.get(), response, true, owner.get()))
            break;
    } while (Transfer(pipe.get(), request, false, owner.get()) && IsValid(request));
    return true;
}

} // namespace
} // namespace exosnap::diagnostics::privileged

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace exosnap::diagnostics::privileged;
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) || !IsElevated())
        return 1;
    const auto location = OpenSiblingHelper(::GetCurrentProcess(), kExecutableName);
    if (!location)
        return 1;

    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&::LocalFree)> args(::CommandLineToArgvW(::GetCommandLineW(), &count),
                                                                 &::LocalFree);
    if (!args || count < 3 || !IsPipeNonce(args.get()[2]))
        return 1;
    const std::wstring_view operation = args.get()[1];
    const std::wstring nonce = args.get()[2];
    if (operation == L"--trace-guard" && count == 6)
        return RunTraceGuard(*location, nonce, args.get()[3], args.get()[4], args.get()[5]) ? 0 : 1;
    if (operation != L"--collect" || count != 5)
        return 1;
    const auto pid = PositiveDecimal(args.get()[3]);
    if (pid == 0 || pid > MAXDWORD)
        return 1;
    return Collect(*location, nonce, static_cast<DWORD>(pid), PositiveDecimal(args.get()[4])) ? 0 : 1;
}
