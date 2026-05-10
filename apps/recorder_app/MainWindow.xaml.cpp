#include "MainWindow.xaml.h"
#include "pages/AdvancedPage.xaml.h"
#include "pages/AudioPage.xaml.h"
#include "pages/DiagnosticsPage.xaml.h"
#include "pages/HotkeysPage.xaml.h"
#include "pages/LogsPage.xaml.h"
#include "pages/OutputPage.xaml.h"
#include "pages/RecordPage.xaml.h"
#include "pages/VideoPage.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::exosnap::implementation {
MainWindow::MainWindow() {
    InitializeComponent();
    NavView().SelectedItem(NavView().MenuItems().GetAt(0));
    ContentFrame().Navigate(xaml_typename<RecordPage>());
}

void MainWindow::NavView_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args) {
    if (auto container = args.SelectedItemContainer()) {
        NavigateToPage(container.Tag().as<hstring>());
    }
}

void MainWindow::NavigateToPage(hstring const& pageTag) {
    if (pageTag == L"Record") {
        ContentFrame().Navigate(xaml_typename<RecordPage>());
    } else if (pageTag == L"Video") {
        ContentFrame().Navigate(xaml_typename<VideoPage>());
    } else if (pageTag == L"Audio") {
        ContentFrame().Navigate(xaml_typename<AudioPage>());
    } else if (pageTag == L"Output") {
        ContentFrame().Navigate(xaml_typename<OutputPage>());
    } else if (pageTag == L"Hotkeys") {
        ContentFrame().Navigate(xaml_typename<HotkeysPage>());
    } else if (pageTag == L"Diagnostics") {
        ContentFrame().Navigate(xaml_typename<DiagnosticsPage>());
    } else if (pageTag == L"Logs") {
        ContentFrame().Navigate(xaml_typename<LogsPage>());
    } else if (pageTag == L"Advanced") {
        ContentFrame().Navigate(xaml_typename<AdvancedPage>());
    }
}
} // namespace winrt::exosnap::implementation
