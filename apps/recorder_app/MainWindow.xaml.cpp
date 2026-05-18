#include "MainWindow.xaml.h"
#include "pages/AdvancedPage.h"
#include "pages/AudioPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/OutputPage.h"
#include "pages/RecordPage.h"
#include "pages/VideoPage.h"
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::exosnap::implementation {
MainWindow::MainWindow() {
    InitializeComponent();
    NavView().SelectedItem(NavView().MenuItems().GetAt(0));
    ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::RecordPage>());
}

void MainWindow::NavView_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args) {
    if (auto container = args.SelectedItemContainer()) {
        NavigateToPage(container.Tag().as<hstring>());
    }
}

void MainWindow::NavigateToPage(hstring const& pageTag) {
    if (pageTag == L"Record") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::RecordPage>());
    } else if (pageTag == L"Video") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::VideoPage>());
    } else if (pageTag == L"Audio") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::AudioPage>());
    } else if (pageTag == L"Output") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::OutputPage>());
    } else if (pageTag == L"Hotkeys") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::HotkeysPage>());
    } else if (pageTag == L"Diagnostics") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::DiagnosticsPage>());
    } else if (pageTag == L"Logs") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::LogsPage>());
    } else if (pageTag == L"Advanced") {
        ContentFrame().Navigate(winrt::xaml_typename<winrt::exosnap::AdvancedPage>());
    }
}
} // namespace winrt::exosnap::implementation

