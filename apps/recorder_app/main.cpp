#include "App.xaml.h"

#include <winrt/Microsoft.UI.Xaml.h>

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto&&) {
            winrt::make<winrt::exosnap::implementation::App>();
        });

    return 0;
}
