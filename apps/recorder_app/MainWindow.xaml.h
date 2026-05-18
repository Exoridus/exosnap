#pragma once
#include "MainWindow.g.h"

namespace winrt::exosnap::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void
    NavView_SelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                             winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);

  private:
    void NavigateToPage(winrt::hstring const& pageTag);
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::exosnap::factory_implementation
