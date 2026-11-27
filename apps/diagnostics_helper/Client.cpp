#include "Channel.h"
#include "Peer.h"
#include "diagnostics/PrivilegedPresentBackend.h"

#include <aclapi.h>
#include <msi.h>
#include <msiquery.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <update/package_verifier.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>

namespace exosnap::diagnostics {
namespace {
using namespace ipc;
namespace fs = std::filesystem;

bool TrustedOwner(PSID sid) {
    PSID installer = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer))
        return false;
    const bool trusted = IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
                         EqualSid(sid, installer);
    LocalFree(installer);
    return trusted;
}

bool Protected(HANDLE object, bool children) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    bool safe = owner && acl && TrustedOwner(owner);
    const DWORD danger = WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
                         GENERIC_ALL | GENERIC_WRITE | (children ? FILE_WRITE_DATA | FILE_APPEND_DATA : 0);
    for (DWORD i = 0; safe && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) {
            safe = false;
            break;
        }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        const bool inherit_only = (ace->Header.AceFlags & INHERIT_ONLY_ACE) != 0;
        if (inherit_only && !children)
            continue;
        if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        safe = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
               (!(ace->Mask & danger) || TrustedOwner(&ace->SidStart) ||
                (inherit_only && IsWellKnownSid(&ace->SidStart, WinCreatorOwnerSid)));
    }
    LocalFree(descriptor);
    return safe;
}

struct LockedPath {
    std::vector<Handle> handles;
    fs::path canonical;

    bool Open(const fs::path& path, bool protected_image, bool directory = false) {
        if (!path.is_absolute() || path.root_name().native().size() != 2 ||
            path.native().find(L'"') != path.native().npos)
            return false;
        auto current = path.root_path();
        for (const auto& part : path.relative_path()) {
            if (part == L"." || part == L".." || part.native().find(L':') != part.native().npos)
                return false;
            current /= part;
            const bool leaf = current == path;
            // Walk top-down, retaining all handles until the OS consumer is done.
            const std::wstring native =
                protected_image ? L"\\\\?\\GLOBALROOT\\GLOBAL??\\" + current.native() : current.native();
            Handle handle(CreateFileW(native.c_str(),
                                      leaf ? GENERIC_READ | READ_CONTROL : FILE_READ_ATTRIBUTES | READ_CONTROL,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            FILE_ATTRIBUTE_TAG_INFO attr{};
            if (!handle || !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attr, sizeof(attr)) ||
                (attr.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((attr.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (!leaf || directory) ||
                (protected_image && !Protected(handle.get(), leaf || current == path.parent_path())))
                return false;
            handles.push_back(std::move(handle));
        }
        if (handles.empty())
            return false;
        // DOS drive mappings are per-logon. Bind the later privileged open to
        // the same local volume even for alternate administrator credentials.
        std::wstring resolved(32768, L'\0');
        const DWORD count =
            GetFinalPathNameByHandleW(handles.back().get(), resolved.data(), static_cast<DWORD>(resolved.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
        if (!count || count >= resolved.size())
            return false;
        resolved.resize(count);
        canonical = std::move(resolved);
        return true;
    }
};

fs::path MachineImage() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT_PATH, nullptr, &raw)))
        return {};
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result / L"ExoSnap Diagnostics" / L"exosnap-diagnostics.exe";
}

bool Verified(const fs::path& path, const char* hash, LockedPath& locked, bool system) {
    return std::string_view(hash).size() == 64 && locked.Open(path, system) &&
           update::VerifyPackageHandle(locked.handles.back().get(), hash) == update::VerifyResult::Ok &&
           ApprovedPublisher(locked.canonical.native(), locked.handles.back().get());
}

// Retain the trusted destination chain while Windows Installer owns its
// transaction. Only a genuinely absent child of a protected directory may be
// created. A junction, an inaccessible entry or a writable installation is a
// terminal refusal, including when the bundled MSI itself is authentic.
bool LockInstallDestination(const fs::path& image, LockedPath& program_files, LockedPath& directory) {
    if (image.empty() || !program_files.Open(image.parent_path().parent_path(), true, true))
        return false;
    const auto folder = program_files.canonical / image.parent_path().filename();
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (!directory.Open(image.parent_path(), true, true))
        return false;
    const auto executable = directory.canonical / image.filename();
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    LockedPath previous;
    if (!previous.Open(image, true))
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(previous.handles.back().get(), &info) && info.nNumberOfLinks == 1 &&
           ApprovedPublisher(previous.canonical.native(), previous.handles.back().get());
}

UINT InstallPackage(HANDLE stop, const fs::path& image) {
    LockedPath program_files;
    LockedPath directory;
    if (!LockInstallDestination(image, program_files, directory))
        return ERROR_INSTALL_FAILURE;
    std::wstring app(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, app.data(), static_cast<DWORD>(app.size()));
    if (!size || size >= app.size())
        return ERROR_INSTALL_FAILURE;
    app.resize(size);
    auto msi = fs::path(app).parent_path() / L"diagnostics" / L"helper.msi";
    LockedPath locked;
    if (!Verified(msi, EXOSNAP_DIAGNOSTICS_MSI_SHA256, locked, false) || WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        return ERROR_INSTALL_FAILURE;
    // Open the verified database in this process before requesting the INSTALL
    // action. Windows Installer owns package elevation and execution. No elevated
    // process is handed a still-unopened package path.
    PMSIHANDLE package;
    if (MsiOpenPackageExW(locked.canonical.c_str(), 0, &package) != ERROR_SUCCESS ||
        WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        return ERROR_INSTALL_FAILURE;
    return MsiDoActionW(package, L"INSTALL");
}

class RemoteBackend final : public IPrivilegedDiagnosticsBackend {
  public:
    explicit RemoteBackend(bool collect_presents) : collect_presents_(collect_presents) {
    }
    DpcLatencyReading Read() const override {
        std::lock_guard lock(mutex_);
        return connected_ && dpc_.metadata.Fresh(std::chrono::steady_clock::now()) ? dpc_ : DpcLatencyReading{};
    }
    PresentTraceOpenResult Open() override {
        stop_ = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        return stop_ ? PresentTraceOpenResult::Opened : PresentTraceOpenResult::Failed;
    }
    void Close() override {
        if (stop_)
            SetEvent(stop_.get());
    }
    int64_t TimestampFrequency() const override {
        return frequency_.load();
    }
    std::optional<PresentProviderState> RuntimeState() const override {
        return state_.load();
    }
    std::vector<TracePresentEvent> Drain() override {
        std::lock_guard lock(mutex_);
        return std::exchange(events_, {});
    }
    std::optional<uint64_t> DrainLostPresentCount() override {
        std::lock_guard lock(mutex_);
        return std::exchange(lost_, uint64_t{0});
    }
    void Consume() override {
        static std::atomic_flag active;
        if (active.test_and_set()) {
            state_.store(PresentProviderState::SessionConflict);
            return;
        }
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com)) {
            state_.store(Run());
            CoUninitialize();
        } else {
            state_.store(PresentProviderState::Failed);
        }
        connected_.store(false);
        active.clear();
    }

  private:
    PresentProviderState Run();
    std::atomic<PresentProviderState> state_{PresentProviderState::Starting};
    std::atomic<int64_t> frequency_{0};
    Handle stop_;
    const bool collect_presents_;
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    DpcLatencyReading dpc_;
    std::vector<TracePresentEvent> events_;
    uint64_t lost_ = 0;
};

PresentProviderState RemoteBackend::Run() {
    if (std::string_view(EXOSNAP_DIAGNOSTICS_IMAGE_SHA256).size() != 64)
        return PresentProviderState::HelperUnavailable;
    const auto image = MachineImage();
    LockedPath verified;
    if (!Verified(image, EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, verified, true)) {
        verified.handles.clear();
        const auto status = InstallPackage(stop_.get(), image);
        if (status == ERROR_INSTALL_USEREXIT || status == ERROR_CANCELLED)
            return PresentProviderState::ConsentDeclined;
        if (status != ERROR_SUCCESS || !Verified(image, EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, verified, true))
            return PresentProviderState::HelperUnavailable;
    }
    GUID id{};
    std::array<wchar_t, 39> guid{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, guid.data(), static_cast<int>(guid.size())))
        return PresentProviderState::Failed;
    std::wstring nonce;
    for (auto c : guid) {
        if (iswxdigit(c))
            nonce += static_cast<wchar_t>(towlower(c));
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // Only elevated administrators may open the client end, including an
    // alternate administrator credential entered on the consent desktop.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;BA)(A;;GA;;;SY)", SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        return PresentProviderState::Failed;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle pipe(CreateNamedPipeW(PipeName(nonce).c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                 65536, 65536, 0, &security));
    LocalFree(descriptor);
    if (!pipe || WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT)
        return PresentProviderState::Failed;
    const auto args = nonce + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
                      std::to_wstring(CreationTime(GetCurrentProcess()));
    const auto directory = verified.canonical.parent_path();
    SHELLEXECUTEINFOW exec{};
    exec.cbSize = sizeof(exec);
    exec.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    exec.lpVerb = L"runas";
    exec.lpFile = verified.canonical.c_str();
    exec.lpParameters = args.c_str();
    exec.lpDirectory = directory.c_str();
    exec.nShow = SW_HIDE;
    if (!ShellExecuteExW(&exec))
        return GetLastError() == ERROR_CANCELLED ? PresentProviderState::ConsentDeclined : PresentProviderState::Failed;
    Handle helper(exec.hProcess);
    Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{};
    operation.hEvent = connected.get();
    if (!helper || !connected)
        return PresentProviderState::Failed;
    if (!ConnectNamedPipe(pipe.get(), &operation)) {
        const auto error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            HANDLE waits[] = {stop_.get(), helper.get(), connected.get()};
            DWORD bytes = 0;
            if (WaitForMultipleObjects(3, waits, FALSE, 10000) != WAIT_OBJECT_0 + 2) {
                CancelIoEx(pipe.get(), &operation);
                GetOverlappedResult(pipe.get(), &operation, &bytes, TRUE);
                return PresentProviderState::Failed;
            }
            if (!GetOverlappedResult(pipe.get(), &operation, &bytes, FALSE))
                return PresentProviderState::Failed;
        } else if (error != ERROR_PIPE_CONNECTED) {
            return PresentProviderState::Failed;
        }
    }
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &pid) || pid != GetProcessId(helper.get()) ||
        WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT)
        return PresentProviderState::Failed;
    Record begin{};
    begin.kind = collect_presents_ ? Kind::Begin : Kind::BeginDpcOnly;
    Record ready{};
    if (!Transfer(pipe.get(), stop_.get(), begin, true, 2000) ||
        !Transfer(pipe.get(), stop_.get(), ready, false, 15000))
        return PresentProviderState::Failed;
    if (ready.kind == Kind::Failure) {
        constexpr std::array states{PresentProviderState::Failed,       PresentProviderState::NotBuilt,
                                    PresentProviderState::AccessDenied, PresentProviderState::SessionConflict,
                                    PresentProviderState::NotSupported, PresentProviderState::Failed};
        state_.store(states[ready.value]);
    } else if (ready.kind == Kind::Ready) {
        frequency_.store(static_cast<int64_t>(ready.value));
        state_.store(PresentProviderState::OpenNoData);
    } else {
        return PresentProviderState::Failed;
    }
    connected_.store(true);
    Record r{};
    while (Transfer(pipe.get(), stop_.get(), r, false, INFINITE)) {
        std::lock_guard lock(mutex_);
        if (r.kind == Kind::Dpc) {
            dpc_.available = true;
            dpc_.max_latency_us = static_cast<double>(r.value) / 1000.0;
            dpc_.avg_latency_us = static_cast<double>(r.window) / 1000.0;
            dpc_.worst_driver = r.driver.data();
            dpc_.metadata.source = "DPC/ISR ETW";
            dpc_.metadata.observed_at = std::chrono::steady_clock::now() - std::chrono::milliseconds(r.process);
        } else if (r.kind == Kind::DpcUnavailable) {
            dpc_ = {};
        } else if (r.kind == Kind::Failure) {
            state_.store(PresentProviderState::Stopped);
        } else if (r.kind == Kind::Lost) {
            lost_ += r.value;
        } else if (r.kind == Kind::Present && events_.size() < 8192) {
            TracePresentEvent event{};
            event.process_id = r.process;
            event.present_qpc = r.value;
            event.hwnd = r.window;
            event.present_mode_code = r.mode;
            event.tearing_flag = (r.flags & 1) != 0;
            event.sync_interval = (r.flags & 2) ? 0 : 1;
            event.discarded = (r.flags & 4) != 0;
            event.related_process_ids.assign(r.related.begin(), r.related.begin() + r.related_count);
            events_.push_back(std::move(event));
        } else {
            return PresentProviderState::Failed;
        }
    }
    return PresentProviderState::Stopped;
}
} // namespace

std::shared_ptr<IPrivilegedDiagnosticsBackend> MakePrivilegedDiagnosticsBackend(bool collect_presents) {
    return std::make_shared<RemoteBackend>(collect_presents);
}
} // namespace exosnap::diagnostics
