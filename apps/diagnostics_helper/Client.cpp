#include "Bootstrap.h"
#include "Channel.h"
#include "FileAcePolicy.h"
#include "FixtureStatus.h"
#include "ImagePath.h"
#include "Peer.h"
#include "RegistryAcePolicy.h"
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include "MsixAcquisition.h"
#endif
#include "diagnostics/PrivilegedPresentBackend.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <update/package_verifier.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
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

bool Protected(HANDLE object, bool children, SE_OBJECT_TYPE type = SE_FILE_OBJECT) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = GetSecurityInfo(object, type, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                        nullptr, &acl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        ReportFixtureStatus("GetSecurityInfo", status);
        return false;
    }
    bool safe = owner && acl && IsValidAcl(acl) && TrustedOwner(owner);
    if (!safe)
        ReportFixtureStatus("untrusted owner or missing DACL", ERROR_ACCESS_DENIED);
    const DWORD danger = type == SE_REGISTRY_KEY
                             ? WRITE_DAC | WRITE_OWNER | DELETE | KEY_SET_VALUE | KEY_CREATE_LINK | KEY_CREATE_SUB_KEY |
                                   GENERIC_WRITE | GENERIC_ALL
                             : WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES |
                                   FILE_WRITE_EA | GENERIC_ALL | GENERIC_WRITE |
                                   (children ? FILE_WRITE_DATA | FILE_APPEND_DATA : 0);
    for (DWORD i = 0; safe && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) {
            safe = false;
            break;
        }
        auto* header = static_cast<ACE_HEADER*>(entry);
        const auto grant = type == SE_REGISTRY_KEY && header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
                               ? FileAceGrant::Invalid
                               : InspectFileAce(*header, danger);
        if (grant == FileAceGrant::Invalid) {
            ReportFixtureStatus("DACL rejected ACE type or rights", header->AceType);
            safe = false;
            break;
        }
        const bool inherit_only = (header->AceFlags & INHERIT_ONLY_ACE) != 0;
        if ((inherit_only && !children) || grant != FileAceGrant::CheckTrustee)
            continue;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        safe = TrustedOwner(&ace->SidStart) || (inherit_only && IsWellKnownSid(&ace->SidStart, WinCreatorOwnerSid)) ||
               (type == SE_REGISTRY_KEY && IsRegistryCreatorTemplate(*header));
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
                ReportFixtureStatus(leaf ? "leaf security descriptor rejected"
                                         : "ancestor security descriptor rejected",
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
        if (!GetFileSizeEx(locked.handles.back().get(), &size) || size.QuadPart <= 0 ||
            size.QuadPart > 64 * 1024 * 1024)
            return false;
    }
    const auto digest = update::VerifyPackageHandle(locked.handles.back().get(), hash);
    if (digest != update::VerifyResult::Ok) {
        ReportFixtureStatus("digest", static_cast<DWORD>(digest));
        return false;
    }
    const bool approved = ApprovedPublisher(locked.canonical.native(), locked.handles.back());
    ReportFixtureStatus(system ? "image publisher accepted" : "MSIX publisher accepted", approved);
    return approved;
}

bool RegistrationValue(const wchar_t* subkey, const wchar_t* value, DWORD type, const void* expected, DWORD bytes) {
    HKEY key = nullptr;
    const std::wstring path(subkey);
    for (size_t separator = path.find(L'\\');; separator = path.find(L'\\', separator + 1)) {
        const auto prefix = path.substr(0, separator);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, prefix.c_str(), REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &key) !=
            ERROR_SUCCESS)
            return false;
        DWORD link_type = 0, link_size = 0;
        const auto link = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &link_type, nullptr, &link_size);
        if (link != ERROR_FILE_NOT_FOUND || !Protected(reinterpret_cast<HANDLE>(key), true, SE_REGISTRY_KEY)) {
            RegCloseKey(key);
            return false;
        }
        if (separator == path.npos)
            break;
        RegCloseKey(key);
        key = nullptr;
    }
    std::vector<BYTE> data(bytes);
    DWORD length = bytes, actual = 0;
    const auto status = RegQueryValueExW(key, value, nullptr, &actual, data.data(), &length);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && actual == type && length == bytes &&
           std::memcmp(data.data(), expected, bytes) == 0;
}

bool BootstrapRegistered(const std::filesystem::path& machine_image) {
    const std::wstring class_key = L"SOFTWARE\\Classes\\CLSID\\{70E064CA-A7D4-43EA-A075-CE4303BF12E5}";
    const std::wstring iface = L"SOFTWARE\\Classes\\Interface\\{F9405F36-29C5-4393-8C0F-74D1B6613E21}";
    const std::wstring typelib = L"SOFTWARE\\Classes\\TypeLib\\{27962A35-7DED-411F-A679-CDD671A2A4B6}\\1.0\\0\\win64";
    // A per-user override has no role in this machine-managed protocol.
    // Its absence here is a conflict check, never authority for elevated activation.
    for (const auto* path : {class_key.c_str(), iface.c_str(), typelib.c_str()}) {
        HKEY user = nullptr;
        const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_WOW64_64KEY, &user);
        if (status == ERROR_SUCCESS)
            RegCloseKey(user);
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
            return false;
    }
    const auto text = [](const std::wstring& key, const wchar_t* name, const std::wstring& expected) {
        return RegistrationValue(key.c_str(), name, REG_SZ, expected.c_str(),
                                 static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)));
    };
    const auto physical = L"\\\\?\\GLOBALROOT\\GLOBAL??\\" + machine_image.native();
    const auto launch = GlobalLaunchPath(physical);
    const DWORD enabled = 1;
    return !launch.empty() && text(class_key, L"LocalizedString", L"@" + physical + L",-101") &&
           RegistrationValue((class_key + L"\\Elevation").c_str(), L"Enabled", REG_DWORD, &enabled, sizeof(enabled)) &&
           text(class_key + L"\\LocalServer32", nullptr, L"\"" + launch + L"\"") &&
           text(class_key + L"\\LocalServer32", L"ServerExecutable", launch) &&
           text(iface + L"\\ProxyStubClsid32", nullptr, L"{00020424-0000-0000-C000-000000000046}") &&
           text(iface + L"\\TypeLib", nullptr, L"{27962A35-7DED-411F-A679-CDD671A2A4B6}") &&
           text(iface + L"\\TypeLib", L"Version", L"1.0") && text(typelib, nullptr, physical);
}

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
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

#endif

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
    const auto machine = MachineImage();
    if (!Verified(machine, EXOSNAP_DIAGNOSTICS_IMAGE_SHA256, verified, true) || !BootstrapRegistered(machine))
        return PresentProviderState::HelperUnavailable;
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return PresentProviderState::Failed;
    const auto words = std::bit_cast<std::array<ULONG, 4>>(id);
    const auto nonce = BootstrapNonce(words);
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
    BIND_OPTS3 options{};
    options.cbStruct = sizeof(options);
    options.dwClassContext = CLSCTX_LOCAL_SERVER;
    Microsoft::WRL::ComPtr<IExoSnapDiagnosticsBootstrap> bootstrap;
    if (WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT)
        return PresentProviderState::Stopped;
    auto status = CoGetObject(kBootstrapMoniker, &options, IID_PPV_ARGS(&bootstrap));
    ReportFixtureStatus("machine bootstrap activation", static_cast<DWORD>(status));
    if (FAILED(status))
        return status == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? PresentProviderState::ConsentDeclined
                                                             : PresentProviderState::HelperUnavailable;
    if (WaitForSingleObject(stop_.get(), 0) != WAIT_TIMEOUT)
        return PresentProviderState::Stopped;
    status = CoSetProxyBlanket(bootstrap.Get(), RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT, nullptr,
                               RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE);
    if (FAILED(status))
        return PresentProviderState::Failed;
    const auto created = CreationTime(GetCurrentProcess());
    LONG restricted = 0;
    status =
        bootstrap->Begin(kBootstrapVersion, WireLong(words[0]), WireLong(words[1]), WireLong(words[2]),
                         WireLong(words[3]), WireLong(GetCurrentProcessId()), WireLong(static_cast<ULONG>(created)),
                         WireLong(static_cast<ULONG>(created >> 32)), &restricted);
    ReportFixtureStatus("authenticated machine bootstrap", static_cast<DWORD>(status));
    if (FAILED(status) || !restricted)
        return PresentProviderState::Failed;
    Handle helper(ULongToHandle(WireBits(restricted)));
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
bool ProbeSignedPackageAcquisition() {
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    LockedPath image;
    return stop && AcquireMsix(stop.get(), image);
}

HRESULT ProbeMachineBootstrap(ULONG version, bool invoke) {
    BIND_OPTS3 options{};
    options.cbStruct = sizeof(options);
    options.dwClassContext = CLSCTX_LOCAL_SERVER;
    Microsoft::WRL::ComPtr<IExoSnapDiagnosticsBootstrap> bootstrap;
    auto result = CoGetObject(kBootstrapMoniker, &options, IID_PPV_ARGS(&bootstrap));
    ReportFixtureStatus("fixture CoGetObject vendor interface", static_cast<DWORD>(result));
    if (FAILED(result) && !invoke) {
        // Distinguish class activation from marshaling this vendor interface.
        Microsoft::WRL::ComPtr<IUnknown> identity;
        const HRESULT base = CoGetObject(kBootstrapMoniker, &options, IID_PPV_ARGS(&identity));
        ReportFixtureStatus("fixture CoGetObject IUnknown control", static_cast<DWORD>(base));
        if (SUCCEEDED(base)) {
            Microsoft::WRL::ComPtr<IExoSnapDiagnosticsBootstrap> typed;
            const HRESULT query = identity.As(&typed);
            ReportFixtureStatus("fixture vendor QueryInterface control", static_cast<DWORD>(query));
        }
    }
    if (FAILED(result) || !invoke)
        return result;
    result = CoSetProxyBlanket(bootstrap.Get(), RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT, nullptr,
                               RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE);
    ReportFixtureStatus("fixture proxy authentication", static_cast<DWORD>(result));
    if (FAILED(result))
        return result;
    const auto created = CreationTime(GetCurrentProcess());
    LONG restricted = 0;
    result = bootstrap->Begin(WireLong(version), 1, 2, 3, 4, WireLong(GetCurrentProcessId()),
                              WireLong(static_cast<ULONG>(created)), WireLong(static_cast<ULONG>(created >> 32)),
                              &restricted);
    ReportFixtureStatus("fixture Begin returned", static_cast<DWORD>(result));
    Handle unexpected(ULongToHandle(WireBits(restricted)));
    return result;
}
#endif

std::shared_ptr<IPrivilegedDiagnosticsBackend> MakePrivilegedDiagnosticsBackend(bool collect_presents) {
    return std::make_shared<RemoteBackend>(collect_presents);
}
} // namespace exosnap::diagnostics
