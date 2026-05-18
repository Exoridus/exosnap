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
#include <sstream>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>

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
        ::exosnap::startup_log::Write(L"window: MainWindow before shell setup");

        nav_list_ = NavList();
        content_frame_ = ContentFrame();
        if (!nav_list_ || !content_frame_) {
            throw winrt::hresult_error(E_FAIL, L"MainWindow shell elements not found");
        }
        ::exosnap::startup_log::Write(L"window: MainWindow got NavList and ContentFrame");

        nav_list_.SelectionChanged({this, &MainWindow::NavList_SelectionChanged});
        if (nav_list_.Items().Size() > 0) {
            nav_list_.SelectedIndex(0);
        }
        ::exosnap::startup_log::Write(L"window: MainWindow shell setup complete");
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

void MainWindow::NavList_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (!nav_list_) {
        return;
    }

    if (auto item = nav_list_.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::ListBoxItem>()) {
        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        if (!tag.empty()) {
            std::string narrow = winrt::to_string(tag);
            std::ostringstream oss;
            oss << "window: NavList selected tag=" << narrow;
            ::exosnap::startup_log::WriteNarrow(oss.str().c_str());
            NavigateToPage(tag);
        }
    }
}

void MainWindow::NavigateToPage(hstring const& pageTag) {
    if (!content_frame_) {
        return;
    }

    try {
        auto page = BuildPageContent(pageTag);
        content_frame_.Content(page);
        bool navigate_ok = page != nullptr;

        std::string narrow = winrt::to_string(pageTag);
        std::ostringstream oss;
        oss << "window: NavigateToPage(" << narrow << ") => " << (navigate_ok ? "true" : "false");
        ::exosnap::startup_log::WriteNarrow(oss.str().c_str());
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("window: NavigateToPage caught hresult_error", ex);
    } catch (std::exception const& ex) {
        ::exosnap::startup_log::WriteNarrow("window: NavigateToPage caught std::exception");
        ::exosnap::startup_log::WriteNarrow(ex.what());
    } catch (...) {
        ::exosnap::startup_log::Write(L"window: NavigateToPage caught unknown exception");
    }
}

winrt::Microsoft::UI::Xaml::UIElement MainWindow::BuildPageContent(hstring const& pageTag) {
    if (pageTag == L"Record") {
        return winrt::make<winrt::exosnap::implementation::RecordPage>();
    }
    if (pageTag == L"Video") {
        return winrt::make<winrt::exosnap::implementation::VideoPage>();
    }
    if (pageTag == L"Audio") {
        return winrt::make<winrt::exosnap::implementation::AudioPage>();
    }
    if (pageTag == L"Output") {
        return winrt::make<winrt::exosnap::implementation::OutputPage>();
    }
    if (pageTag == L"Hotkeys") {
        return winrt::make<winrt::exosnap::implementation::HotkeysPage>();
    }
    if (pageTag == L"Diagnostics") {
        return winrt::make<winrt::exosnap::implementation::DiagnosticsPage>();
    }
    if (pageTag == L"Logs") {
        return winrt::make<winrt::exosnap::implementation::LogsPage>();
    }
    if (pageTag == L"Advanced") {
        return winrt::make<winrt::exosnap::implementation::AdvancedPage>();
    }

    return nullptr;
}
} // namespace winrt::exosnap::implementation


