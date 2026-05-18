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
#include <winrt/Windows.UI.Text.h>
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
        ::exosnap::startup_log::Write(L"window: MainWindow before shell setup");

        nav_list_ = NavList();
        content_frame_ = ContentFrame();
        if (!nav_list_ || !content_frame_) {
            throw winrt::hresult_error(E_FAIL, L"MainWindow shell elements not found");
        }
        ::exosnap::startup_log::Write(L"window: MainWindow got NavList and ContentFrame");

        nav_list_.SelectionChanged({this, &MainWindow::NavList_SelectionChanged});
        if (nav_list_.Items().Size() > 1) {
            nav_list_.SelectedIndex(1);
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
            std::string narrow(tag.begin(), tag.end());
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
        content_frame_.Content(BuildSectionContent(pageTag));
        bool navigate_ok = true;

        std::string narrow(pageTag.begin(), pageTag.end());
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

winrt::Microsoft::UI::Xaml::UIElement MainWindow::BuildSectionContent(hstring const& pageTag) {
    auto root = winrt::Microsoft::UI::Xaml::Controls::StackPanel{};
    root.Spacing(10);
    root.Margin(winrt::Microsoft::UI::Xaml::Thickness{24, 18, 24, 18});

    auto title = winrt::Microsoft::UI::Xaml::Controls::TextBlock{};
    title.Text(pageTag);
    title.FontSize(30);
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    root.Children().Append(title);

    auto subtitle = winrt::Microsoft::UI::Xaml::Controls::TextBlock{};
    subtitle.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
    subtitle.Opacity(0.85);

    if (pageTag == L"Record") {
        subtitle.Text(L"Record workflow shell: live preview and runtime controls are being stabilized.");
    } else if (pageTag == L"Video") {
        subtitle.Text(L"Video settings panel scaffold.");
    } else if (pageTag == L"Audio") {
        subtitle.Text(L"Audio source and track settings scaffold.");
    } else if (pageTag == L"Output") {
        subtitle.Text(L"Output container/codec and destination scaffold.");
    } else if (pageTag == L"Hotkeys") {
        subtitle.Text(L"Hotkey configuration scaffold.");
    } else if (pageTag == L"Diagnostics") {
        subtitle.Text(L"Diagnostics blockers and capability summary scaffold.");
    } else if (pageTag == L"Logs") {
        subtitle.Text(L"Runtime log viewer scaffold.");
    } else if (pageTag == L"Advanced") {
        subtitle.Text(L"Advanced expert settings scaffold.");
    } else {
        subtitle.Text(L"Section scaffold.");
    }
    root.Children().Append(subtitle);

    auto details = winrt::Microsoft::UI::Xaml::Controls::TextBlock{};
    details.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
    details.Opacity(0.7);
    details.Text(L"Navigation is now wired for all MVP sections so you can preview layout and flow.");
    root.Children().Append(details);

    return root;
}
} // namespace winrt::exosnap::implementation


