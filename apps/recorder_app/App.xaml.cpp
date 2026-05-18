#include "App.xaml.h"
#include "MainWindow.xaml.h"
#if __has_include("App.xaml.g.hpp")
#include "App.xaml.g.hpp"
#endif

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

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
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    // Capture UI dispatcher before any async work
    ui_dispatcher_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

    window = make<exosnap::implementation::MainWindow>();
    window.Activate();

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

