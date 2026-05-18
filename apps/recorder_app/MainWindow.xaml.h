#pragma once
#include "MainWindow.g.h"

namespace winrt::exosnap::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void NavList_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);

  private:
    winrt::Microsoft::UI::Xaml::Controls::ListBox nav_list_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Frame content_frame_{nullptr};

    winrt::Microsoft::UI::Xaml::UIElement BuildPageContent(winrt::hstring const& pageTag);
    void NavigateToPage(winrt::hstring const& pageTag);
};
} // namespace winrt::exosnap::implementation

namespace winrt::exosnap::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
} // namespace winrt::exosnap::factory_implementation
