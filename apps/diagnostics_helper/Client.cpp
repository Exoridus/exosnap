#include "Channel.h"
#include "FileAcePolicy.h"
#include "FixtureStatus.h"
#include "ImagePath.h"
#include "MsixAcquisition.h"
#include "Peer.h"
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include "StartingGuardian.h"
#endif
#include "diagnostics/PrivilegedPresentBackend.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <update/package_verifier.h>

#include <algorithm>
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
    const auto status = GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, nullptr, &acl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        ReportFixtureStatus("GetSecurityInfo", status);
        return false;
    }
    bool safe = owner && acl && IsValidAcl(acl) && TrustedOwner(owner);
    if (!safe)
        ReportFixtureStatus("untrusted owner or missing DACL", ERROR_ACCESS_DENIED);
    const DWORD danger = WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
                         GENERIC_ALL | GENERIC_WRITE | (children ? FILE_WRITE_DATA | FILE_APPEND_DATA : 0);
    for (DWORD i = 0; safe && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) {
            safe = false;
            break;
        }
        auto* header = static_cast<ACE_HEADER*>(entry);
        const auto grant = InspectFileAce(*header, danger);
        if (grant == FileAceGrant::Invalid) {
            ReportFixtureStatus("DACL rejected ACE type or rights", header->AceType);
            safe = false;
            break;
        }
        const bool inherit_only = (header->AceFlags & INHERIT_ONLY_ACE) != 0;
        if ((inherit_only && !children) || grant != FileAceGrant::CheckTrustee)
            continue;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        safe = TrustedOwner(&ace->SidStart) || (inherit_only && IsWellKnownSid(&ace->SidStart, WinCreatorOwnerSid));
        if (!safe)
            ReportFixtureStatus("DACL rejected ACE rights", ace->Mask);
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
            if (!handle || !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attr, sizeof(attr))) {
                ReportFixtureStatus(leaf ? "leaf open or attributes" : "ancestor open or attributes", GetLastError());
                return false;
            }
            if ((attr.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((attr.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (!leaf || directory)) {
                ReportFixtureStatus("unexpected file attributes", attr.FileAttributes);
                return false;
            }
            if (protected_image && !Protected(handle.get(), leaf || current == path.parent_path())) {
                ReportFixtureStatus(leaf ? "leaf security descriptor rejected" : "ancestor security descriptor rejected",
                                    ERROR_ACCESS_DENIED);
                return false;
            }
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
        if (!count || count >= resolved.size()) {
            ReportFixtureStatus("GetFinalPathNameByHandleW", GetLastError());
            return false;
        }
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
    if (std::string_view(hash).size() != 64) {
        ReportFixtureStatus("missing build digest", ERROR_INVALID_DATA);
        return false;
    }
    if (!locked.Open(path, system)) {
        ReportFixtureStatus(system ? "protected image path" : "MSIX path", GetLastError());
        return false;
    }
    if (!system) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(locked.handles.back().get(), &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
            return false;
    }
    const auto digest = update::VerifyPackageHandle(locked.handles.back().get(), hash);
    if (digest != update::VerifyResult::Ok) {
        ReportFixtureStatus("digest", static_cast<DWORD>(digest));
        return false;
    }
    const bool approved = ApprovedPublisher(locked.canonical.native(), locked.handles.back().get());
    ReportFixtureStatus(system ? "image publisher accepted" : "MSIX publisher accepted", approved);
    return approved;
}

bool LockFootprint(LockedPath& image, const fs::path& directory, const wchar_t* filename,
                   std::span<const unsigned char> expected) {
    // The image verification has already locked every ancestor in this directory.
    // Resolve the sibling from the exact physical parent, never another DOS mapping.
    Handle file(CreateFileW((directory / filename).c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER length{};
    if (!file || !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !Protected(file.get(), true) || !GetFileSizeEx(file.get(), &length))
        return false;
    if (length.QuadPart != static_cast<LONGLONG>(expected.size())) {
        NoteFootprintMismatch();
        return false;
    }
    std::vector<unsigned char> actual(expected.size());
    DWORD read = 0;
    if (!ReadFile(file.get(), actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr) ||
        read != actual.size())
        return false;
    if (!std::equal(actual.begin(), actual.end(), expected.begin())) {
        NoteFootprintMismatch();
        return false;
    }
    image.handles.push_back(std::move(file));
    return true;
}

bool AcquireMsix(HANDLE stop, LockedPath& image) {
    std::wstring app(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, app.data(), static_cast<DWORD>(app.size()));
    if (!size || size >= app.size())
        return {};
    app.resize(size);
    const auto source = fs::path(app).parent_path() / L"diagnostics" / L"helper.msix";
    LockedPath locked;
    if (!Verified(source, EXOSNAP_DIAGNOSTICS_MSIX_SHA256, locked, false))
        return {};
    const auto verify = [&](const fs::path& executable, std::span<const unsigned char> manifest,
                            std::span<const unsigned char> block_map) {
        if (!Verified(executable, EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, image, true))
            return false;
        std::wstring native(32768, L'\0');
        const auto length = GetFinalPathNameByHandleW(image.handles.back().get(), native.data(),
                                                      static_cast<DWORD>(native.size()), VOLUME_NAME_NT);
        if (!length || length >= native.size())
            return false;
        native.resize(length);
        const fs::path physical = GlobalLocalVolumePath(native);
        return !physical.empty() && LockFootprint(image, physical.parent_path(), L"AppxManifest.xml", manifest) &&
               LockFootprint(image, physical.parent_path(), L"AppxBlockMap.xml", block_map);
    };
    return AcquireRegisteredMsix(locked.handles.back().get(), source, stop, verify);
}

class RemoteBackend final : public IPrivilegedDiagnosticsBackend {
  public:
    explicit RemoteBackend(bool collect_presents) : collect_presents_(collect_presents) {
    }
    DiagnosticsHelperState HelperState() const override {
        return helper_state_.load();
    }
    DpcLatencyReading Read() const override {
        std::lock_guard lock(mutex_);
        return HelperState() == DiagnosticsHelperState::Connected &&
                       dpc_.metadata.Fresh(std::chrono::steady_clock::now())
                   ? dpc_
                   : DpcLatencyReading{};
    }
    PresentTraceOpenResult Open() override {
        stop_ = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_)
            helper_state_.store(DiagnosticsHelperState::Failed);
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
            helper_state_.store(DiagnosticsHelperState::Failed);
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
        switch (state_.load()) {
        case PresentProviderState::ConsentDeclined:
            helper_state_.store(DiagnosticsHelperState::ConsentDeclined);
            break;
        case PresentProviderState::HelperUnavailable:
            helper_state_.store(DiagnosticsHelperState::Unavailable);
            break;
        case PresentProviderState::Stopped:
            helper_state_.store(DiagnosticsHelperState::Stopped);
            break;
        default:
            helper_state_.store(DiagnosticsHelperState::Failed);
            break;
        }
        active.clear();
    }

  private:
    PresentProviderState Run();
    std::atomic<PresentProviderState> state_{PresentProviderState::Starting};
    std::atomic<int64_t> frequency_{0};
    Handle stop_;
    const bool collect_presents_;
    std::atomic<DiagnosticsHelperState> helper_state_{DiagnosticsHelperState::Starting};
    mutable std::mutex mutex_;
    DpcLatencyReading dpc_;
    std::vector<TracePresentEvent> events_;
    uint64_t lost_ = 0;
};

PresentProviderState RemoteBackend::Run() {
    if (std::string_view(EXOSNAP_DIAGNOSTICS_IMAGE_SHA256).size() != 64)
        return PresentProviderState::HelperUnavailable;
    LockedPath verified;
    if (!Verified(MachineImage(), EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, verified, true)) {
        verified.handles.clear();
        if (!AcquireMsix(stop_.get(), verified))
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
    if (!pipe || WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT) {
        ReportFixtureStatus("pipe create or cancelled", GetLastError());
        return PresentProviderState::Failed;
    }
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
    if (WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT)
        return PresentProviderState::Stopped;
    if (!ShellExecuteExW(&exec)) {
        const auto error = GetLastError();
        ReportFixtureStatus("ShellExecuteExW", error);
        return error == ERROR_CANCELLED ? PresentProviderState::ConsentDeclined : PresentProviderState::Failed;
    }
    ReportFixtureStatus("ShellExecuteExW", ERROR_SUCCESS);
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
            const auto wait = WaitForMultipleObjects(3, waits, FALSE, 10000);
            if (wait != WAIT_OBJECT_0 + 2) {
                ReportFixtureStatus("pipe connect wait", wait);
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
                DWORD exit_code = 0;
                if (GetExitCodeProcess(helper.get(), &exit_code))
                    ReportFixtureStatus("helper exit at connect", exit_code);
#endif
                CancelIoEx(pipe.get(), &operation);
                GetOverlappedResult(pipe.get(), &operation, &bytes, TRUE);
                return PresentProviderState::Failed;
            }
            if (!GetOverlappedResult(pipe.get(), &operation, &bytes, FALSE)) {
                ReportFixtureStatus("pipe connect completion", GetLastError());
                return PresentProviderState::Failed;
            }
        } else if (error != ERROR_PIPE_CONNECTED) {
            ReportFixtureStatus("ConnectNamedPipe", error);
            return PresentProviderState::Failed;
        }
    }
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &pid) || pid != GetProcessId(helper.get()) ||
        WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT) {
        ReportFixtureStatus("pipe peer identity rejected", ERROR_ACCESS_DENIED);
        return PresentProviderState::Failed;
    }
    Record begin{};
    begin.kind = collect_presents_ ? Kind::Begin : Kind::BeginDpcOnly;
    Record ready{};
    if (!Transfer(pipe.get(), stop_.get(), begin, true, 2000)) {
        ReportFixtureStatus("initial pipe write rejected", ERROR_INVALID_DATA);
        return PresentProviderState::Failed;
    }
    if (!Transfer(pipe.get(), stop_.get(), ready, false, 15000)) {
        ReportFixtureStatus("initial pipe response rejected", ERROR_INVALID_DATA);
        return PresentProviderState::Failed;
    }
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
    helper_state_.store(DiagnosticsHelperState::Connected);
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

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
void ProbeInstalledHelperLaunch() {
    LockedPath locked;
    if (!Verified(MachineImage(), EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, locked, true)) {
        ReportFixtureStatus("launch matrix rejected image", ERROR_ACCESS_DENIED);
        return;
    }
    std::wstring native(32768, L'\0');
    const auto count = GetFinalPathNameByHandleW(locked.handles.back().get(), native.data(),
                                                 static_cast<DWORD>(native.size()), VOLUME_NAME_NT);
    if (!count || count >= native.size())
        return;
    native.resize(count);
    const fs::path global = GlobalLocalVolumePath(native);
    if (global.empty())
        return;
    struct Spelling {
        fs::path path;
        const char* label;
    };
    auto device = [](const fs::path& path) {
        auto name = path.native();
        if (!name.starts_with(LR"(\\?\)"))
            return fs::path{};
        name[2] = L'.';
        return fs::path(name);
    };
    const std::array spellings{Spelling{locked.canonical, "volume GUID launch matrix"},
                               Spelling{global, "global native launch matrix"},
                               Spelling{device(locked.canonical), "device volume GUID launch matrix"},
                               Spelling{device(global), "device global native launch matrix"},
                               Spelling{MachineImage(), "fixture-only DOS positive control"}};
    const auto probe = [](const fs::path& path, const char* label, const wchar_t* parent, HKEY class_key) {
        SHELLEXECUTEINFOW exec{};
        exec.cbSize = sizeof(exec);
        exec.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        exec.lpVerb = L"runas";
        exec.lpFile = path.c_str();
        exec.lpDirectory = parent;
        if (class_key) {
            exec.fMask |= SEE_MASK_CLASSKEY;
            exec.hkeyClass = class_key;
        }
        exec.nShow = SW_HIDE;
        const BOOL launched = ShellExecuteExW(&exec);
        const DWORD status = launched ? ERROR_SUCCESS : GetLastError();
        ReportFixtureStatus(label, status);
        ReportFixtureStatus("matrix supplies working directory", parent != nullptr);
        if (launched && exec.hProcess) {
            StartingGuardian child{Handle(exec.hProcess)};
            const auto wait = WaitForSingleObject(child.get(), 5000);
            ReportFixtureStatus("matrix child wait", wait);
            DWORD exit_code = 0;
            if (GetExitCodeProcess(child.get(), &exit_code))
                ReportFixtureStatus("matrix child exit", exit_code);
        }
    };
    for (const auto& [path, label] : spellings) {
        if (path.empty())
            continue;
        const auto parent = path.parent_path();
        probe(path, label, nullptr, nullptr);
        probe(path, label, parent.c_str(), nullptr);
    }
    HKEY class_key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\exefile", 0, KEY_READ | KEY_WOW64_64KEY,
                                      &class_key);
    if (opened == ERROR_SUCCESS) {
        probe(locked.canonical, "HKLM exefile volume GUID", nullptr, class_key);
        probe(global, "HKLM exefile global native", nullptr, class_key);
        RegCloseKey(class_key);
    } else {
        ReportFixtureStatus("HKLM exefile class unavailable", static_cast<DWORD>(opened));
    }
}
#endif

std::shared_ptr<IPrivilegedDiagnosticsBackend> MakePrivilegedDiagnosticsBackend(bool collect_presents) {
    return std::make_shared<RemoteBackend>(collect_presents);
}
} // namespace exosnap::diagnostics
