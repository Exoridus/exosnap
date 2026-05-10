#pragma once
#include "App.xaml.g.h"

namespace winrt::exosnap::implementation {
struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

  private:
    winrt::Microsoft::UI::Xaml::Window window{nullptr};
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct App : AppT<App, implementation::App> {};
} // namespace winrt::exosnap::factory_implementation
