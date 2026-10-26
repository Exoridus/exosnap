#include <windows.h>

#include <msxml6.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <dutil.h>

#include <dictutil.h>
#include <strutil.h>

#include <BalBaseBootstrapperApplication.h>
#include <BalBaseBootstrapperApplicationProc.h>

#include "../SetupBridge.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace exosnap::setup;
constexpr UINT DetectMessage = WM_APP + 1;
constexpr UINT PlanMessage = WM_APP + 2;
constexpr UINT ApplyMessage = WM_APP + 3;
constexpr UINT QuitMessage = WM_APP + 4;
constexpr UINT LaunchMessage = WM_APP + 5;

class Bootstrapper final : public CBalBaseBootstrapperApplication {
  public:
    Bootstrapper(IBootstrapperEngine* engine, const BOOTSTRAPPER_COMMAND& command)
        : CBalBaseBootstrapperApplication(engine), engine_(engine), action_(command.action), display_(command.display),
          workingDirectory_(command.wzBootstrapperWorkingFolder),
          host_{this,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                command.display == BOOTSTRAPPER_DISPLAY_PASSIVE ||
                    (command.action != BOOTSTRAPPER_ACTION_INSTALL && command.action != BOOTSTRAPPER_ACTION_MODIFY &&
                     command.action != BOOTSTRAPPER_ACTION_REPAIR && command.action != BOOTSTRAPPER_ACTION_UNINSTALL),
                command.action == BOOTSTRAPPER_ACTION_UNINSTALL,
                command.action == BOOTSTRAPPER_ACTION_REPAIR,
                Subscribe,
                Detect,
                Apply,
                Cancel,
                Quit,
                Launch,
                Restart} {
        version_ = Variable(L"WixBundleVersion");
        logPath_ = Variable(L"WixBundleLog");
        wchar_t path[32768]{};
        SIZE_T length = _countof(path);
        if (SUCCEEDED(engine_->FormatString(L"[ProgramFiles64Folder]ExoSnap", path, &length))) {
            installPath_ = path;
        }
        host_.version = version_.c_str();
        host_.log_path = logPath_.c_str();
        host_.install_path = installPath_.c_str();
        host_.runtime_path = workingDirectory_.c_str();
    }

    ~Bootstrapper() override {
        if (worker_.joinable()) {
            PostThreadMessageW(workerId_, WM_QUIT, 0, 0);
            worker_.join();
        }
        if (ui_.joinable()) {
            ui_.join();
        }
    }

    STDMETHODIMP OnStartup() override {
        worker_ = std::thread([this] { Run(); });
        return S_OK;
    }

    STDMETHODIMP OnShutdown(BOOTSTRAPPER_SHUTDOWN_ACTION* action) override {
        *action = restartRequested_ ? BOOTSTRAPPER_SHUTDOWN_ACTION_RESTART : BOOTSTRAPPER_SHUTDOWN_ACTION_NONE;
        PostThreadMessageW(workerId_, WM_QUIT, 0, 0);
        return S_OK;
    }

    STDMETHODIMP OnDetectPackageComplete(LPCWSTR id, HRESULT status, BOOTSTRAPPER_PACKAGE_STATE state, BOOL) override {
        if (SUCCEEDED(status) && wcscmp(id, L"ExoSnapMsi") == 0) {
            installed_ = state == BOOTSTRAPPER_PACKAGE_STATE_PRESENT;
        }
        return S_OK;
    }

    STDMETHODIMP OnDetectRelatedBundle(LPCWSTR, BOOTSTRAPPER_RELATION_TYPE relation, LPCWSTR, BOOL, LPCWSTR version,
                                       BOOL, BOOL*) override {
        if (relation == BOOTSTRAPPER_RELATION_UPGRADE) {
            RememberPreviousVersion(version);
        }
        return S_OK;
    }

    STDMETHODIMP OnDetectRelatedMsiPackage(LPCWSTR id, LPCWSTR, LPCWSTR, BOOL, LPCWSTR version,
                                           BOOTSTRAPPER_RELATED_OPERATION operation, BOOL*) override {
        if (wcscmp(id, L"ExoSnapMsi") == 0 && operation == BOOTSTRAPPER_RELATED_OPERATION_MAJOR_UPGRADE) {
            RememberPreviousVersion(version);
        }
        return S_OK;
    }

    STDMETHODIMP OnDetectComplete(HRESULT status, BOOL) override {
        if (FAILED(status)) {
            Finish(status, false);
            return S_OK;
        }
        Emit(SetupEventKind::Detected, status);
        if (display_ != BOOTSTRAPPER_DISPLAY_FULL ||
            (action_ != BOOTSTRAPPER_ACTION_INSTALL && action_ != BOOTSTRAPPER_ACTION_MODIFY &&
             action_ != BOOTSTRAPPER_ACTION_REPAIR && action_ != BOOTSTRAPPER_ACTION_UNINSTALL)) {
            PostThreadMessageW(workerId_, PlanMessage, 0, 0);
        }
        return S_OK;
    }

    STDMETHODIMP OnPlanComplete(HRESULT status) override {
        if (FAILED(status)) {
            Finish(status, false);
        } else {
            PostThreadMessageW(workerId_, ApplyMessage, 0, 0);
        }
        return S_OK;
    }

    STDMETHODIMP OnApplyBegin(DWORD, BOOL* cancel) override {
        canCancel_ = true;
        *cancel |= cancelRequested_.load() ? TRUE : FALSE;
        Emit(SetupEventKind::Applying);
        return S_OK;
    }

    STDMETHODIMP OnCacheAcquireProgress(LPCWSTR, LPCWSTR, DWORD64, DWORD64, DWORD, BOOL* cancel) override {
        // Only OnProgress reports whole-bundle progress. Cache percentages use a different denominator.
        *cancel |= canCancel_ && cancelRequested_;
        return S_OK;
    }

    STDMETHODIMP OnProgress(DWORD, DWORD overall, BOOL* cancel) override {
        *cancel |= canCancel_ && cancelRequested_;
        Emit(SetupEventKind::Progress, S_OK, overall);
        return S_OK;
    }

    STDMETHODIMP OnExecuteBegin(DWORD, BOOL* cancel) override {
        *cancel |= cancelRequested_.load() ? TRUE : FALSE;
        canCancel_ = false;
        Emit(SetupEventKind::Applying);
        return S_OK;
    }

    STDMETHODIMP OnExecutePackageBegin(LPCWSTR id, BOOL, BOOTSTRAPPER_ACTION_STATE, INSTALLUILEVEL, BOOL,
                                       BOOL*) override {
        Emit(SetupEventKind::PackageBegin, S_OK, 0, id);
        return S_OK;
    }

    STDMETHODIMP OnExecuteFilesInUse(LPCWSTR, DWORD count, LPCWSTR* files, int, BOOTSTRAPPER_FILES_IN_USE_TYPE source,
                                     int* result) override {
        std::wstring names;
        for (DWORD index = 0; index < count; ++index) {
            if (!names.empty()) {
                names += L"\n";
            }
            names += files[index];
        }
        engine_->Log(
            BOOTSTRAPPER_LOG_LEVEL_ERROR,
            L"Setup cannot continue while application files are in use. Close the applications and run Setup again.");
        Emit(SetupEventKind::FilesInUse, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), 0, names.c_str());
        // Restart Manager's IDOK would close applications that may still be recording.
        *result = source == BOOTSTRAPPER_FILES_IN_USE_TYPE_MSI ? IDABORT : IDCANCEL;
        return S_OK;
    }

    STDMETHODIMP OnExecutePackageComplete(LPCWSTR id, HRESULT status, BOOTSTRAPPER_APPLY_RESTART,
                                          BOOTSTRAPPER_EXECUTEPACKAGECOMPLETE_ACTION,
                                          BOOTSTRAPPER_EXECUTEPACKAGECOMPLETE_ACTION* action) override {
        *action = BOOTSTRAPPER_EXECUTEPACKAGECOMPLETE_ACTION_NONE;
        Emit(SetupEventKind::PackageComplete, status, 0, id);
        return S_OK;
    }

    STDMETHODIMP OnError(BOOTSTRAPPER_ERROR_TYPE, LPCWSTR, DWORD code, LPCWSTR text, DWORD, DWORD, LPCWSTR*,
                         int recommendation, int* result) override {
        *result = recommendation;
        Emit(SetupEventKind::Error, HRESULT_FROM_WIN32(code), 0, text);
        return S_OK;
    }

    STDMETHODIMP OnApplyComplete(HRESULT status, BOOTSTRAPPER_APPLY_RESTART restart, BOOTSTRAPPER_APPLYCOMPLETE_ACTION,
                                 BOOTSTRAPPER_APPLYCOMPLETE_ACTION* action) override {
        *action = BOOTSTRAPPER_APPLYCOMPLETE_ACTION_NONE;
        Finish(status, restart != BOOTSTRAPPER_APPLY_RESTART_NONE);
        return S_OK;
    }

  private:
    struct ApplyRequest {
        SetupAction action;
        bool desktopShortcut;
        bool removeUserData;
        HWND window;
    };

    std::wstring Variable(LPCWSTR name) {
        wchar_t value[32768]{};
        SIZE_T length = _countof(value);
        return SUCCEEDED(engine_->GetVariableString(name, value, &length)) ? value : L"";
    }

    void RememberPreviousVersion(LPCWSTR version) {
        int comparison = 0;
        if (version && SUCCEEDED(engine_->CompareVersions(version, version_.c_str(), &comparison)) && comparison < 0) {
            if (previousVersion_.empty() ||
                (SUCCEEDED(engine_->CompareVersions(version, previousVersion_.c_str(), &comparison)) &&
                 comparison > 0)) {
                previousVersion_ = version;
            }
        }
    }

    void Emit(SetupEventKind kind, HRESULT status = S_OK, DWORD progress = 0, LPCWSTR text = nullptr,
              bool restart = false) {
        std::lock_guard lock(callbackMutex_);
        if (callback_) {
            const SetupEvent event{
                kind,
                status,
                progress,
                installed_,
                restart,
                canCancel_,
                text,
                installed_ ? version_.c_str() : previousVersion_.empty() ? nullptr : previousVersion_.c_str()};
            callback_(callbackContext_, &event);
        }
    }

    void Finish(HRESULT status, bool restart) {
        canCancel_ = false;
        busy_ = false;
        exitCode_ = FAILED(status) ? (HRESULT_FACILITY(status) == FACILITY_WIN32 ? HRESULT_CODE(status)
                                                                                 : static_cast<DWORD>(status))
                    : restart      ? ERROR_SUCCESS_REBOOT_REQUIRED
                                   : ERROR_SUCCESS;
        Emit(SetupEventKind::Completed, status, SUCCEEDED(status) ? 100 : 0, nullptr, restart);
        if (display_ == BOOTSTRAPPER_DISPLAY_NONE || display_ == BOOTSTRAPPER_DISPLAY_EMBEDDED) {
            PostThreadMessageW(workerId_, QuitMessage, exitCode_, 0);
        }
    }

    void LaunchFailed(HRESULT status) {
        const auto text = L"exosnap.exe (" + std::to_wstring(HRESULT_CODE(status)) + L")";
        Emit(SetupEventKind::Error, status, 0, text.c_str());
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        workerId_ = GetCurrentThreadId();
        if (display_ == BOOTSTRAPPER_DISPLAY_NONE || display_ == BOOTSTRAPPER_DISPLAY_EMBEDDED) {
            PostThreadMessageW(workerId_, DetectMessage, 0, 0);
        } else {
            ui_ = std::thread([this] { RunUi(); });
        }
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            HRESULT status = S_OK;
            switch (message.message) {
            case DetectMessage:
                status = engine_->Detect(window_);
                break;
            case PlanMessage: {
                std::unique_ptr<ApplyRequest> request;
                {
                    std::lock_guard lock(requestMutex_);
                    request = std::move(pendingRequest_);
                    busy_ = true;
                }
                if (request) {
                    action_ = request->action == SetupAction::Uninstall ? BOOTSTRAPPER_ACTION_UNINSTALL
                              : request->action == SetupAction::Repair  ? BOOTSTRAPPER_ACTION_REPAIR
                                                                        : BOOTSTRAPPER_ACTION_INSTALL;
                    window_ = request->window;
                    status = engine_->SetVariableNumeric(L"ExoSnapDesktopShortcut", request->desktopShortcut);
                    if (SUCCEEDED(status)) {
                        status = engine_->SetVariableNumeric(L"ExoSnapRemoveUserData",
                                                             action_ == BOOTSTRAPPER_ACTION_UNINSTALL &&
                                                                 request->removeUserData);
                    }
                }
                if (SUCCEEDED(status)) {
                    status = engine_->Plan(action_);
                }
                break;
            }
            case ApplyMessage:
                status = engine_->Apply(window_);
                break;
            case LaunchMessage: {
                const auto executable = installPath_ + L"\\exosnap.exe";
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(reinterpret_cast<HWND>(message.wParam),
                                                                            L"open", executable.c_str(), nullptr,
                                                                            installPath_.c_str(), SW_SHOWNORMAL));
                if (result <= 32) {
                    LaunchFailed(HRESULT_FROM_WIN32(result ? static_cast<DWORD>(result) : ERROR_GEN_FAILURE));
                }
                break;
            }
            case QuitMessage:
                if (!busy_ && !quitting_) {
                    quitting_ = true;
                    engine_->Quit(message.wParam ? static_cast<DWORD>(message.wParam) : exitCode_.load());
                }
                break;
            default:
                TranslateMessage(&message);
                DispatchMessageW(&message);
                break;
            }
            if (FAILED(status)) {
                Finish(status, false);
            }
        }
        CoUninitialize();
    }

    void RunUi() {
        // Burn extracts every private dependency before the first dynamic-CRT DLL is loaded.
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        const auto directory = AddDllDirectory(workingDirectory_.c_str());
        const auto dllPath = workingDirectory_ + L"\\exosnap-setup-ui.dll";
        const auto module = LoadLibraryExW(dllPath.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                               LOAD_LIBRARY_SEARCH_USER_DIRS);
        const auto run = module ? reinterpret_cast<RunSetupUi>(GetProcAddress(module, "RunSetupUi")) : nullptr;
        if (run) {
            engine_->CloseSplashScreen();
            const auto result = run(&host_);
            Subscribe(this, nullptr, nullptr);
            PostThreadMessageW(workerId_, QuitMessage, result, 0);
        } else {
            engine_->Log(BOOTSTRAPPER_LOG_LEVEL_ERROR, L"Cannot load the private Setup UI runtime.");
            PostThreadMessageW(workerId_, QuitMessage, ERROR_DLL_INIT_FAILED, 0);
        }
        if (module) {
            FreeLibrary(module);
        }
        if (directory) {
            RemoveDllDirectory(directory);
        }
    }

    static void Subscribe(void* context, SetupEventCallback callback, void* callbackContext) {
        auto& self = *static_cast<Bootstrapper*>(context);
        std::lock_guard lock(self.callbackMutex_);
        self.callback_ = callback;
        self.callbackContext_ = callbackContext;
    }
    static void Detect(void* context) {
        auto& self = *static_cast<Bootstrapper*>(context);
        PostThreadMessageW(self.workerId_, DetectMessage, 0, 0);
    }
    static void Apply(void* context, SetupAction action, bool shortcut, bool remove, std::uintptr_t window) {
        auto& self = *static_cast<Bootstrapper*>(context);
        HRESULT status = S_OK;
        {
            std::lock_guard lock(self.requestMutex_);
            if (self.busy_ || self.pendingRequest_) {
                return;
            }
            self.pendingRequest_ =
                std::make_unique<ApplyRequest>(ApplyRequest{action, shortcut, remove, reinterpret_cast<HWND>(window)});
            self.busy_ = true;
            self.cancelRequested_ = false;
            if (!PostThreadMessageW(self.workerId_, PlanMessage, 0, 0)) {
                const auto error = GetLastError();
                status = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
                self.pendingRequest_.reset();
            }
        }
        if (FAILED(status)) {
            self.Finish(status, false);
        }
    }
    static void Cancel(void* context) {
        auto& self = *static_cast<Bootstrapper*>(context);
        if (self.canCancel_) {
            self.cancelRequested_ = true;
        }
    }
    static void Quit(void* context, std::uint32_t code) {
        auto& self = *static_cast<Bootstrapper*>(context);
        PostThreadMessageW(self.workerId_, QuitMessage, code, 0);
    }
    static void Launch(void* context, std::uintptr_t window) {
        auto& self = *static_cast<Bootstrapper*>(context);
        PostThreadMessageW(self.workerId_, LaunchMessage, window, 0);
    }
    static void Restart(void* context) {
        auto& self = *static_cast<Bootstrapper*>(context);
        if (!self.busy_ && self.exitCode_ == ERROR_SUCCESS_REBOOT_REQUIRED) {
            self.restartRequested_ = true;
            PostThreadMessageW(self.workerId_, QuitMessage, ERROR_SUCCESS_REBOOT_REQUIRED, 0);
        }
    }

    IBootstrapperEngine* engine_;
    BOOTSTRAPPER_ACTION action_;
    BOOTSTRAPPER_DISPLAY display_;
    std::wstring workingDirectory_, version_, logPath_, installPath_, previousVersion_;
    SetupHost host_;
    std::thread worker_, ui_;
    DWORD workerId_ = 0;
    HWND window_ = nullptr;
    bool quitting_ = false;
    std::atomic<bool> busy_{false}, canCancel_{false}, cancelRequested_{false}, installed_{false};
    std::atomic<DWORD> exitCode_{0};
    std::atomic<bool> restartRequested_{false};
    std::mutex callbackMutex_;
    std::mutex requestMutex_;
    std::unique_ptr<ApplyRequest> pendingRequest_;
    SetupEventCallback callback_ = nullptr;
    void* callbackContext_ = nullptr;
};

Bootstrapper* application = nullptr;
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI BootstrapperApplicationCreate(const BOOTSTRAPPER_CREATE_ARGS* args,
                                                                              BOOTSTRAPPER_CREATE_RESULTS* results) {
    IBootstrapperEngine* engine = nullptr;
    const auto status = BalInitializeFromCreateArgs(args, &engine);
    if (FAILED(status)) {
        return status;
    }
    application = new (std::nothrow) Bootstrapper(engine, *args->pCommand);
    engine->Release();
    if (!application) {
        BalUninitialize();
        return E_OUTOFMEMORY;
    }
    const auto initialized = application->Initialize(args);
    if (FAILED(initialized)) {
        application->Release();
        application = nullptr;
        BalUninitialize();
        return initialized;
    }
    results->pfnBootstrapperApplicationProc = BalBaseBootstrapperApplicationProc;
    results->pvBootstrapperApplicationProcContext = static_cast<IBootstrapperApplication*>(application);
    return S_OK;
}

extern "C" __declspec(dllexport) void WINAPI BootstrapperApplicationDestroy(const BOOTSTRAPPER_DESTROY_ARGS*,
                                                                            BOOTSTRAPPER_DESTROY_RESULTS*) {
    if (application) {
        application->Release();
        application = nullptr;
    }
    BalUninitialize();
}
