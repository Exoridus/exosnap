#include "diagnostics/PrivilegedDiagnosticsClient.h"

#include "diagnostics/PrivilegedDiagnosticsPath.h"
#include "diagnostics/PrivilegedDiagnosticsProtocol.h"

#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

#include <chrono>
#include <limits>

namespace exosnap::diagnostics {
using namespace privileged;

struct PrivilegedDiagnosticsClient::Connection {
    Handle stop{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle ended{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle present_ended{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<PrivilegedDiagnosticsState> status{PrivilegedDiagnosticsState::Starting};
    std::mutex mutex;
    std::vector<TracePresentEvent> presents;
    int64_t frequency = 0;
    uint64_t lost = 0;
    DpcLatencyReading dpc;
    bool dpc_trace_open = false;
    bool present_available = false;
    bool present_used = false; // GUI thread only
};

namespace {

using Connection = PrivilegedDiagnosticsClient::Connection;

struct LocalDelete {
    void operator()(void* memory) const {
        ::LocalFree(memory);
    }
};

Handle OpenPipe(const std::wstring& nonce) {
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token))
        return {};
    const Handle token(raw_token);
    DWORD bytes = 0;
    ::GetTokenInformation(token.get(), TokenLogonSid, nullptr, 0, &bytes);
    if (bytes == 0 || bytes > 65536)
        return {};
    std::vector<BYTE> groups_buffer(bytes);
    if (!::GetTokenInformation(token.get(), TokenLogonSid, groups_buffer.data(), bytes, &bytes))
        return {};
    const auto* logon = reinterpret_cast<const TOKEN_GROUPS*>(groups_buffer.data());
    LPWSTR raw_sid = nullptr;
    if (logon->GroupCount != 1 || !::ConvertSidToStringSidW(logon->Groups[0].Sid, &raw_sid))
        return {};
    const std::unique_ptr<wchar_t, LocalDelete> sid(raw_sid);
    // Explicit read/write rights omit FILE_CREATE_PIPE_INSTANCE. Administrators
    // also covers an alternate-credential UAC launch in this Windows session.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;0x100183;;;BA)(A;;0x100183;;;" + std::wstring(sid.get()) + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw_descriptor,
                                                                nullptr))
        return {};
    const std::unique_ptr<void, LocalDelete> descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
    const std::wstring name = kPipePrefix + nonce;
    return Handle(::CreateNamedPipeW(name.c_str(),
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                     sizeof(Response), sizeof(Request), 0, &security));
}

void Publish(Connection& state, const Response& response) {
    std::lock_guard lock(state.mutex);
    constexpr size_t max_pending = kMaxPresents * 8;
    if (response.present_state != PresentStatus::Open || state.presents.size() + response.present_count > max_pending) {
        if (state.present_available || response.present_state == PresentStatus::Unavailable)
            ::SetEvent(state.present_ended.get());
        state.present_available = false;
        state.presents.clear();
    } else if (::WaitForSingleObject(state.present_ended.get(), 0) == WAIT_TIMEOUT) {
        state.present_available = true;
        state.frequency = response.frequency;
        state.lost += std::min(response.lost, std::numeric_limits<uint64_t>::max() - state.lost);
        for (uint32_t i = 0; i < response.present_count; ++i) {
            const auto& row = response.presents[i];
            auto& event = state.presents.emplace_back();
            event.process_id = row.pid;
            event.hwnd = row.hwnd;
            event.present_qpc = row.qpc;
            event.present_mode_code = row.mode;
            event.sync_interval = row.sync_interval;
            event.tearing_flag = (row.flags & 1U) != 0;
            event.discarded = (row.flags & 2U) != 0;
            event.related_process_ids.assign(row.related.begin(), row.related.begin() + row.related_count);
        }
    }
    state.dpc = {};
    state.dpc_trace_open = response.dpc_open != 0;
    const uint64_t qpc = QpcNow();
    const int64_t frequency = QpcFrequency();
    if (response.dpc_qpc != 0 && response.dpc_qpc <= qpc && frequency > 0) {
        const auto age =
            std::chrono::duration<double>(static_cast<double>(qpc - response.dpc_qpc) / static_cast<double>(frequency));
        if (age > std::chrono::seconds(15))
            return;
        auto& reading = state.dpc;
        reading.available = true;
        reading.max_latency_us = response.dpc_max_us;
        reading.avg_latency_us = response.dpc_average_us;
        reading.worst_driver = response.dpc_driver.data();
        reading.metadata.source = "DPC/ISR ETW";
        reading.metadata.observed_at =
            std::chrono::steady_clock::now() - std::chrono::ceil<std::chrono::steady_clock::duration>(age);
    }
}

PrivilegedDiagnosticsState Run(Connection& state, bool capture_present) {
    const auto location = OpenProtectedHelper();
    if (!location)
        return PrivilegedDiagnosticsState::Missing;
    const auto nonce = NewNonce();
    if (nonce.empty())
        return PrivilegedDiagnosticsState::Failed;
    Handle pipe = OpenPipe(nonce);
    const auto started_at = ProcessBirth(::GetCurrentProcess());
    if (!pipe || started_at == 0)
        return PrivilegedDiagnosticsState::Failed;
    if (::WaitForSingleObject(state.stop.get(), 0) != WAIT_TIMEOUT)
        return PrivilegedDiagnosticsState::Idle;

    const std::wstring args =
        L"--collect " + nonce + L" " + std::to_wstring(::GetCurrentProcessId()) + L" " + std::to_wstring(started_at);
    SHELLEXECUTEINFOW shell{};
    shell.cbSize = sizeof(shell);
    shell.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    shell.lpVerb = L"runas";
    shell.lpFile = location->executable.c_str();
    shell.lpParameters = args.c_str();
    shell.lpDirectory = location->directory.c_str();
    shell.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&shell))
        return ::GetLastError() == ERROR_CANCELLED ? PrivilegedDiagnosticsState::Declined
                                                   : PrivilegedDiagnosticsState::Failed;
    const Handle helper(shell.hProcess);
    ULONG client_pid = 0;
    if (!helper || !Connect(pipe.get(), helper.get(), state.stop.get()) ||
        !::GetNamedPipeClientProcessId(pipe.get(), &client_pid) || client_pid != ::GetProcessId(helper.get()) ||
        !SameSession(client_pid) || ::WaitForSingleObject(helper.get(), 0) != WAIT_TIMEOUT)
        return PrivilegedDiagnosticsState::Failed;

    Request request{};
    Response response{};
    HANDLE signals[]{state.stop.get(), helper.get()};
    do {
        request.present =
            capture_present && ::WaitForSingleObject(state.present_ended.get(), 0) == WAIT_TIMEOUT ? 1U : 0U;
        if (!Transfer(pipe.get(), request, true, helper.get(), state.stop.get()) ||
            !Transfer(pipe.get(), response, false, helper.get(), state.stop.get()) || !IsValid(response))
            break;
        Publish(state, response);
        state.status.store(PrivilegedDiagnosticsState::Running);
    } while (::WaitForMultipleObjects(2, signals, FALSE, 250) == WAIT_TIMEOUT);
    ::DisconnectNamedPipe(pipe.get());
    ::SetEvent(state.present_ended.get());
    state.status.store(PrivilegedDiagnosticsState::Failed);
    // Keep new enable requests out until the old endpoint has a chance to
    // close owned sessions. Cleanup itself is enforced by the elevated owner.
    ::WaitForSingleObject(helper.get(), kIoTimeoutMs);
    return PrivilegedDiagnosticsState::Failed;
}

class RemotePresentBackend final : public IPresentTraceBackend {
  public:
    explicit RemotePresentBackend(std::shared_ptr<Connection> state) : state_(std::move(state)) {
    }

    PresentTraceOpenResult Open() override {
        std::lock_guard lock(state_->mutex);
        return stop_ && state_->present_available ? PresentTraceOpenResult::Opened : PresentTraceOpenResult::Failed;
    }
    void Consume() override {
        HANDLE signals[]{stop_.get(), state_->present_ended.get(), state_->ended.get()};
        ::WaitForMultipleObjects(3, signals, FALSE, INFINITE);
    }
    void Close() override {
        ::SetEvent(stop_.get());
    }
    int64_t TimestampFrequency() const override {
        std::lock_guard lock(state_->mutex);
        return state_->frequency;
    }
    std::vector<TracePresentEvent> Drain() override {
        std::lock_guard lock(state_->mutex);
        return std::exchange(state_->presents, {});
    }
    std::optional<uint64_t> DrainLostPresentCount() override {
        std::lock_guard lock(state_->mutex);
        return std::exchange(state_->lost, 0);
    }

  private:
    std::shared_ptr<Connection> state_;
    Handle stop_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

} // namespace

PrivilegedDiagnosticsClient::~PrivilegedDiagnosticsClient() {
    Stop();
}

void PrivilegedDiagnosticsClient::Start(bool capture_present) {
    if (connection_ && ::WaitForSingleObject(connection_->ended.get(), 0) != WAIT_OBJECT_0)
        return;
    auto state = std::make_shared<Connection>();
    if (!state->stop || !state->ended || !state->present_ended)
        return;
    connection_ = state;
    try {
        std::thread([state, capture_present]() {
            const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            auto outcome = PrivilegedDiagnosticsState::Failed;
            if (SUCCEEDED(com)) {
                try {
                    outcome = Run(*state, capture_present);
                } catch (const std::exception&) {
                    // Handle unwinding disconnects the elevated collector.
                }
                ::CoUninitialize();
            }
            state->status.store(outcome);
            ::SetEvent(state->ended.get());
        }).detach();
    } catch (const std::system_error&) {
        state->status.store(PrivilegedDiagnosticsState::Failed);
        ::SetEvent(state->ended.get());
    }
}

void PrivilegedDiagnosticsClient::Stop() {
    if (connection_)
        ::SetEvent(connection_->stop.get());
}

PrivilegedDiagnosticsState PrivilegedDiagnosticsClient::Status() const {
    if (!connection_)
        return PrivilegedDiagnosticsState::Idle;
    if (::WaitForSingleObject(connection_->stop.get(), 0) == WAIT_OBJECT_0)
        return ::WaitForSingleObject(connection_->ended.get(), 0) == WAIT_OBJECT_0
                   ? PrivilegedDiagnosticsState::Idle
                   : PrivilegedDiagnosticsState::Stopping;
    return connection_->status.load();
}

DpcLatencyReading PrivilegedDiagnosticsClient::Read() const {
    if (Status() != PrivilegedDiagnosticsState::Running)
        return {};
    std::lock_guard lock(connection_->mutex);
    return connection_->dpc;
}

bool PrivilegedDiagnosticsClient::KernelTraceOpen() const {
    if (Status() != PrivilegedDiagnosticsState::Running)
        return false;
    std::lock_guard lock(connection_->mutex);
    return connection_->dpc_trace_open;
}

std::shared_ptr<IPresentTraceBackend> PrivilegedDiagnosticsClient::MakePresentBackend() const {
    if (Status() != PrivilegedDiagnosticsState::Running || connection_->present_used)
        return nullptr;
    std::lock_guard lock(connection_->mutex);
    if (!connection_->present_available)
        return nullptr;
    connection_->present_used = true;
    return std::make_shared<RemotePresentBackend>(connection_);
}

} // namespace exosnap::diagnostics
