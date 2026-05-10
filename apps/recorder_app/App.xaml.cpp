#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::exosnap::implementation {
App::App() {
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window = make<exosnap::implementation::MainWindow>();
    window.Activate();
}
} // namespace winrt::exosnap::implementation
