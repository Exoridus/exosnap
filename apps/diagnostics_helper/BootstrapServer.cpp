#include "Bootstrap.h"
#include "Peer.h"

#include <rpcasync.h>
#include <sddl.h>
#include <wrl.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace exosnap::diagnostics::ipc {
namespace {
using namespace Microsoft::WRL;

struct Activation {
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex;
    bool closed = false;
    std::wstring nonce;
    Handle caller;
    uint64_t creation = 0;
};

class Bootstrap final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExoSnapDiagnosticsBootstrap> {
public:
    explicit Bootstrap(std::shared_ptr<Activation> activation) : activation_(std::move(activation)) {}

    HRESULT STDMETHODCALLTYPE Begin(LONG version, LONG a, LONG b, LONG c, LONG d, LONG pid,
                                    LONG low, LONG high, LONG* result) override {
        if (!result)
            return E_POINTER;
        *result = 0;
        if (version != kBootstrapVersion)
            return E_INVALIDARG;
        const ULONG requested_pid = WireBits(pid);
        RPC_CALL_ATTRIBUTES_V2_W call{};
        call.Version = 2;
        call.Flags = RPC_QUERY_CLIENT_PID | RPC_QUERY_IS_CLIENT_LOCAL;
        if (RpcServerInqCallAttributesW(nullptr, &call) != RPC_S_OK || call.IsClientLocal != rcclLocal ||
            call.NullSession || HandleToULong(call.ClientPID) != requested_pid || !requested_pid)
            return E_ACCESSDENIED;
        Handle caller(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE,
                                  FALSE, requested_pid));
        DWORD caller_session = 0, own_session = 0;
        const uint64_t creation = (static_cast<uint64_t>(WireBits(high)) << 32) | WireBits(low);
        if (!caller || !creation || CreationTime(caller.get()) != creation || !ApprovedClient(caller.get()) ||
            !ProcessIdToSessionId(requested_pid, &caller_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || caller_session != own_session ||
            WaitForSingleObject(caller.get(), 0) != WAIT_TIMEOUT)
            return E_ACCESSDENIED;
        std::lock_guard lock(activation_->mutex);
        if (activation_->closed || activation_->caller)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        try {
            activation_->nonce = BootstrapNonce({WireBits(a), WireBits(b), WireBits(c), WireBits(d)});
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        HANDLE restricted = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), caller.get(), &restricted,
                             SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
            return HRESULT_FROM_WIN32(GetLastError());
        activation_->caller = std::move(caller);
        activation_->creation = creation;
        *result = WireLong(HandleToULong(restricted));
        SetEvent(activation_->ready.get());
        return S_OK;
    }

private:
    std::shared_ptr<Activation> activation_;
};

class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    explicit Factory(std::shared_ptr<Activation> activation) : activation_(std::move(activation)) {}

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        if (issued_.exchange(true))
            return CLASS_E_CLASSNOTAVAILABLE;
        auto object = Make<Bootstrap>(activation_);
        return object ? object.CopyTo(iid, result) : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return E_NOTIMPL; }

private:
    std::shared_ptr<Activation> activation_;
    std::atomic<bool> issued_{false};
};

int Serve(CollectFunction collect) {
    // Permit local interactive over-the-shoulder callers to reach the bounded
    // method; it independently authenticates the process before any operation.
    SECURITY_DESCRIPTOR descriptor{};
    PSECURITY_DESCRIPTOR relative = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYG:SYD:(A;;0x3;;;SY)(A;;0x3;;;IU)(A;;0x3;;;BA)", SDDL_REVISION_1, &relative, nullptr))
        return 1;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    PSID owner = nullptr, group = nullptr;
    const bool built = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
        GetSecurityDescriptorDacl(relative, &present, &dacl, &defaulted) && present && dacl &&
        SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) &&
        GetSecurityDescriptorOwner(relative, &owner, &defaulted) &&
        SetSecurityDescriptorOwner(&descriptor, owner, FALSE) &&
        GetSecurityDescriptorGroup(relative, &group, &defaulted) &&
        SetSecurityDescriptorGroup(&descriptor, group, FALSE);
    const auto security = built ? CoInitializeSecurity(&descriptor, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NO_CUSTOM_MARSHAL | EOAC_DISABLE_AAA, nullptr) : E_FAIL;
    LocalFree(relative);
    if (FAILED(security))
        return 1;
    auto activation = std::make_shared<Activation>();
    auto factory = Make<Factory>(activation);
    DWORD cookie = 0;
    if (!activation->ready || !factory || FAILED(CoRegisterClassObject(kBootstrapClass, factory.Get(),
            CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &cookie)))
        return 1;
    const auto wait = WaitForSingleObject(activation->ready.get(), 15000);
    CoRevokeClassObject(cookie);
    {
        std::lock_guard lock(activation->mutex);
        activation->closed = true;
        if (wait != WAIT_OBJECT_0 || !activation->caller)
            return 1;
    }
    return collect(activation->nonce, GetProcessId(activation->caller.get()), activation->creation);
}
} // namespace

int RunBootstrapServer(CollectFunction collect) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;
    int status = 1;
    try {
        status = Serve(collect);
    } catch (const std::bad_alloc&) {
        status = 1;
    }
    CoUninitialize();
    return status;
}
} // namespace exosnap::diagnostics::ipc
