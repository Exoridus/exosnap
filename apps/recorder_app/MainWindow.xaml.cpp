#include "MainWindow.xaml.h"
#include "pages/AdvancedPage.h"
#include "pages/AudioPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/OutputPage.h"
#include "pages/RecordPage.h"
#include "pages/VideoPage.h"
#include "startup_log.h"
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif

#include <exception>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::exosnap::implementation {
MainWindow::MainWindow() {
    ::exosnap::startup_log::Write(L"window: MainWindow ctor entry");
    try {
        ::exosnap::startup_log::Write(L"window: MainWindow before InitializeComponent");
        InitializeComponent();
        ::exosnap::startup_log::Write(L"window: MainWindow after InitializeComponent");
        ::exosnap::startup_log::Write(L"window: MainWindow before preview content setup");

        auto root = Content().try_as<winrt::Microsoft::UI::Xaml::Controls::Grid>();
        if (!root) {
            throw winrt::hresult_error(E_FAIL, L"MainWindow root Grid not found");
        }
        ::exosnap::startup_log::Write(L"window: MainWindow got root Grid");
        auto title = winrt::Microsoft::UI::Xaml::Controls::TextBlock{};
        title.Text(L"Recorder App UI Preview");
        title.FontSize(24.0);
        title.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        title.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        root.Children().Append(title);
        ::exosnap::startup_log::Write(L"window: MainWindow preview content setup complete");
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("window: MainWindow ctor caught hresult_error", ex);
        throw;
    } catch (std::exception const& ex) {
        ::exosnap::startup_log::WriteNarrow("window: MainWindow ctor caught std::exception");
        ::exosnap::startup_log::WriteNarrow(ex.what());
        throw;
    } catch (...) {
        ::exosnap::startup_log::Write(L"window: MainWindow ctor caught unknown exception");
        throw;
    }
}

void MainWindow::NavView_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args) {
    if (auto container = args.SelectedItemContainer()) {
        NavigateToPage(container.Tag().as<hstring>());
    }
}

void MainWindow::NavigateToPage(hstring const& pageTag) {
    if (!content_frame_) {
        return;
    }

    if (pageTag == L"Record") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::RecordPage>());
    } else if (pageTag == L"Video") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::VideoPage>());
    } else if (pageTag == L"Audio") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::AudioPage>());
    } else if (pageTag == L"Output") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::OutputPage>());
    } else if (pageTag == L"Hotkeys") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::HotkeysPage>());
    } else if (pageTag == L"Diagnostics") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::DiagnosticsPage>());
    } else if (pageTag == L"Logs") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::LogsPage>());
    } else if (pageTag == L"Advanced") {
        content_frame_.Navigate(winrt::xaml_typename<winrt::exosnap::AdvancedPage>());
    }
}
} // namespace winrt::exosnap::implementation


