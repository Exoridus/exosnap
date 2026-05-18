#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "startup_log.h"
#if __has_include("App.xaml.g.hpp")
#include "App.xaml.g.hpp"
#endif

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <exception>
#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::exosnap::implementation {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

// ---------------------------------------------------------------------------
// App implementation
// ---------------------------------------------------------------------------

App::App() {
    ::exosnap::startup_log::Write(L"app: App::App entry");
    // C++/WinRT create_and_initialize() invokes InitializeComponent() after
    // construction; avoid calling it manually here to prevent double-init.
    ::exosnap::startup_log::Write(L"app: App::App relying on create_and_initialize for InitializeComponent");
    unhandled_exception_token_ = UnhandledException(
        [this](IInspectable const&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args) {
            (void)this;
            ::exosnap::startup_log::Write(L"app: UnhandledException event fired");
            {
                std::ostringstream oss;
                oss << "app: UnhandledException HRESULT=0x"
                    << std::hex << std::uppercase
                    << static_cast<uint32_t>(args.Exception().value);
                ::exosnap::startup_log::WriteNarrow(oss.str().c_str());
            }
            ::exosnap::startup_log::Write(args.Message().c_str());
            // Temporary diagnostic escape hatch to see whether startup can continue.
            args.Handled(true);
            ::exosnap::startup_log::Write(L"app: UnhandledException marked handled=true");
        });
    ::exosnap::startup_log::Write(L"app: App::App registered UnhandledException handler");
}

App::~App() {
    if (unhandled_exception_token_.value != 0) {
        UnhandledException(unhandled_exception_token_);
    }
    ::exosnap::startup_log::Write(L"app: App::~App");
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    ::exosnap::startup_log::Write(L"app: OnLaunched entry");
    try {
        ::exosnap::startup_log::Write(L"app: OnLaunched before dispatcher capture");
        // Capture UI dispatcher before any async work
        ui_dispatcher_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        ::exosnap::startup_log::Write(L"app: OnLaunched after dispatcher capture");

        ::exosnap::startup_log::Write(L"app: OnLaunched before MainWindow make");
        window = make<exosnap::implementation::MainWindow>();
        ::exosnap::startup_log::Write(L"app: OnLaunched after MainWindow make");

        ::exosnap::startup_log::Write(L"app: OnLaunched before window.Activate");
        window.Activate();
        ::exosnap::startup_log::Write(L"app: OnLaunched after window.Activate");

        ::exosnap::startup_log::Write(L"app: OnLaunched before capability thread startup");
        // Start capability discovery on a jthread (never detached)
        capability_thread_ = std::jthread([this](std::stop_token stop) {
            try {
                auto caps = std::make_unique<::exosnap::capability::CapabilitySet>(
                    ::exosnap::capability::CapabilityBuilder::BuildFromHardwareQuery());

                ::exosnap::capability::SettingsResolver resolver(*caps);
                auto validation = resolver.ValidateConfig(::exosnap::capability::UserRecorderConfig{});

                if (stop.stop_requested()) return;

                // Marshal result to UI thread
                ui_dispatcher_.TryEnqueue(
                    [this, c = std::move(caps), v = std::move(validation)]() mutable {
                        capabilities_        = std::move(c);
                        primary_validation_  = std::move(v);
                        if (on_capability_ready_) {
                            on_capability_ready_(*capabilities_, *primary_validation_);
                        }
                    });
            } catch (const std::exception& e) {
                std::wstring msg = ToWide(e.what());
                if (stop.stop_requested()) return;
                ui_dispatcher_.TryEnqueue([this, m = std::move(msg)]() mutable {
                    if (on_capability_failed_) {
                        on_capability_failed_(std::move(m));
                    }
                });
            } catch (...) {
                if (stop.stop_requested()) return;
                ui_dispatcher_.TryEnqueue([this]() {
                    if (on_capability_failed_) {
                        on_capability_failed_(L"Unknown error during capability initialization");
                    }
                });
            }
        });
        ::exosnap::startup_log::Write(L"app: OnLaunched after capability thread startup");
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("app: OnLaunched caught hresult_error", ex);
        throw;
    } catch (std::exception const& ex) {
        ::exosnap::startup_log::WriteNarrow("app: OnLaunched caught std::exception");
        ::exosnap::startup_log::WriteNarrow(ex.what());
        throw;
    } catch (...) {
        ::exosnap::startup_log::Write(L"app: OnLaunched caught unknown exception");
        throw;
    }
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void App::RegisterCapabilityCallbacks(
    CapabilityReadyCallback on_ready,
    CapabilityFailedCallback on_failed) {

    // If already resolved, fire immediately on the caller's thread (UI thread).
    if (capabilities_) {
        on_ready(*capabilities_, *primary_validation_);
    } else {
        on_capability_ready_  = std::move(on_ready);
        on_capability_failed_ = std::move(on_failed);
    }
}

// ---------------------------------------------------------------------------
// Safe accessors
// ---------------------------------------------------------------------------

bool App::CapabilitiesReady() const noexcept {
    return capabilities_ != nullptr;
}

const ::exosnap::capability::CapabilitySet* App::Capabilities() const noexcept {
    return capabilities_.get();
}

const ::exosnap::capability::ResolveResult* App::PrimaryValidation() const noexcept {
    return primary_validation_.has_value() ? &*primary_validation_ : nullptr;
}

} // namespace winrt::exosnap::implementation


