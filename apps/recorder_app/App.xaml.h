#pragma once
#include "App.g.h"

#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/base.h>

namespace winrt::exosnap::implementation {

struct App : AppT<App> {
    App();
    ~App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    // Capability callbacks that RecordPage registers.
    using CapabilityReadyCallback  = std::function<void(
        const ::exosnap::capability::CapabilitySet&,
        const ::exosnap::capability::ResolveResult&)>;
    using CapabilityFailedCallback = std::function<void(std::wstring)>;

    void RegisterCapabilityCallbacks(
        CapabilityReadyCallback on_ready,
        CapabilityFailedCallback on_failed);

    // Safe accessors
    bool                                          CapabilitiesReady()   const noexcept;
    const ::exosnap::capability::CapabilitySet*   Capabilities()        const noexcept;
    const ::exosnap::capability::ResolveResult*   PrimaryValidation()   const noexcept;

  private:
    winrt::Microsoft::UI::Xaml::Window window{ nullptr };

    // Capability ownership
    std::unique_ptr<::exosnap::capability::CapabilitySet> capabilities_;
    std::optional<::exosnap::capability::ResolveResult>   primary_validation_;
    std::jthread                                           capability_thread_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue     ui_dispatcher_{ nullptr };
    winrt::event_token                                      unhandled_exception_token_{};

    CapabilityReadyCallback  on_capability_ready_;
    CapabilityFailedCallback on_capability_failed_;
};

} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct App : AppT<App, implementation::App> {};
} // namespace winrt::exosnap::factory_implementation
