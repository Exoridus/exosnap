#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "startup_log.h"
#include "pages/AdvancedPage.h"
#include "pages/AudioPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/OutputPage.h"
#include "pages/RecordPage.h"
#include "pages/VideoPage.h"

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.Foundation.h>
#include <sstream>

namespace winrt::exosnap::implementation {

namespace {

void ThrowNoUnloadableObjects() {
    throw winrt::hresult_invalid_argument{L"No unloadable objects."};
}

void ThrowNoUnloadableObjectsToDisconnect() {
    throw winrt::hresult_invalid_argument{L"No unloadable objects to disconnect."};
}

void LogConnectorCall(char const* method, int32_t connection_id) {
    std::ostringstream oss;
    oss << method << " id=" << connection_id;
    ::exosnap::startup_log::WriteNarrow(oss.str().c_str());
}

template <typename TObject>
void LoadComponent(TObject const& object, wchar_t const* uri) {
    winrt::Microsoft::UI::Xaml::Application::LoadComponent(
        object,
        winrt::Windows::Foundation::Uri{uri},
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ComponentResourceLocation::Application);
}

template <typename TControl>
TControl FindNameFromWindow(winrt::Microsoft::UI::Xaml::Window const& window, wchar_t const* name) {
    if (auto root = window.Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
        return root.FindName(winrt::hstring{name}).try_as<TControl>();
    }
    return nullptr;
}

template <typename TControl>
TControl FindNameFromElement(winrt::Microsoft::UI::Xaml::FrameworkElement const& root, wchar_t const* name) {
    return root.FindName(winrt::hstring{name}).try_as<TControl>();
}

} // namespace

#define EXO_DEFINE_XAML_COMPAT_STUBS(TEMPLATE_NAME, TYPE_NAME)                                                   \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::Connect(                                                                       \
        int32_t connection_id,                                                                                    \
        winrt::Windows::Foundation::IInspectable const&) {                                                        \
        LogConnectorCall(#TEMPLATE_NAME "<" #TYPE_NAME ">::Connect", connection_id);                             \
        _contentLoaded = true;                                                                                    \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    winrt::Microsoft::UI::Xaml::Markup::IComponentConnector TEMPLATE_NAME<TYPE_NAME>::GetBindingConnector(       \
        int32_t connection_id,                                                                                    \
        winrt::Windows::Foundation::IInspectable const&) {                                                        \
        LogConnectorCall(#TEMPLATE_NAME "<" #TYPE_NAME ">::GetBindingConnector", connection_id);                 \
        return nullptr;                                                                                           \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::UnloadObject(                                                                  \
        winrt::Microsoft::UI::Xaml::DependencyObject const&) {                                                    \
        ::exosnap::startup_log::WriteNarrow(#TEMPLATE_NAME "<" #TYPE_NAME ">::UnloadObject");                    \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::DisconnectUnloadedObject(int32_t connection_id) {                             \
        LogConnectorCall(#TEMPLATE_NAME "<" #TYPE_NAME ">::DisconnectUnloadedObject", connection_id);            \
    }

#define EXO_DEFINE_SIMPLE_XAML_COMPAT(TEMPLATE_NAME, TYPE_NAME, URI_LITERAL)                                     \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::InitializeComponent() {                                                        \
        if (!_contentLoaded) {                                                                                    \
            _contentLoaded = true;                                                                                \
            LoadComponent(*this, URI_LITERAL);                                                                    \
        }                                                                                                         \
    }                                                                                                             \
    EXO_DEFINE_XAML_COMPAT_STUBS(TEMPLATE_NAME, TYPE_NAME)

template <>
void AppT<App>::InitializeComponent() {
    ::exosnap::startup_log::Write(L"compat: AppT::InitializeComponent entry");
    try {
        if (!_contentLoaded) {
            _contentLoaded = true;
            ::exosnap::startup_log::Write(L"compat: AppT before LoadComponent(App.xaml)");
            winrt::Microsoft::UI::Xaml::Application::LoadComponent(
                *this,
                winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"},
                winrt::Microsoft::UI::Xaml::Controls::Primitives::ComponentResourceLocation::Application);
            ::exosnap::startup_log::Write(L"compat: AppT after LoadComponent(App.xaml)");
        }
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("compat: AppT::InitializeComponent caught hresult_error", ex);
        throw;
    } catch (...) {
        ::exosnap::startup_log::Write(L"compat: AppT::InitializeComponent caught unknown exception");
        throw;
    }
}

EXO_DEFINE_XAML_COMPAT_STUBS(AppT, App)

template <>
void MainWindowT<MainWindow>::InitializeComponent() {
    ::exosnap::startup_log::Write(L"compat: MainWindowT::InitializeComponent entry");
    try {
        if (_contentLoaded) {
            ::exosnap::startup_log::Write(L"compat: MainWindowT already loaded");
            return;
        }

        _contentLoaded = true;
        ::exosnap::startup_log::Write(L"compat: MainWindowT before LoadComponent(MainWindow.xaml)");
        LoadComponent(*this, L"ms-appx:///MainWindow.xaml");
        ::exosnap::startup_log::Write(L"compat: MainWindowT after LoadComponent(MainWindow.xaml)");
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("compat: MainWindowT::InitializeComponent caught hresult_error", ex);
        throw;
    } catch (...) {
        ::exosnap::startup_log::Write(L"compat: MainWindowT::InitializeComponent caught unknown exception");
        throw;
    }
}

EXO_DEFINE_XAML_COMPAT_STUBS(MainWindowT, MainWindow)

EXO_DEFINE_SIMPLE_XAML_COMPAT(AdvancedPageT, AdvancedPage, L"ms-appx:///pages/AdvancedPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(AudioPageT, AudioPage, L"ms-appx:///pages/AudioPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(DiagnosticsPageT, DiagnosticsPage, L"ms-appx:///pages/DiagnosticsPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(HotkeysPageT, HotkeysPage, L"ms-appx:///pages/HotkeysPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(LogsPageT, LogsPage, L"ms-appx:///pages/LogsPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(OutputPageT, OutputPage, L"ms-appx:///pages/OutputPage.xaml")
EXO_DEFINE_SIMPLE_XAML_COMPAT(VideoPageT, VideoPage, L"ms-appx:///pages/VideoPage.xaml")

template <>
void RecordPageT<RecordPage>::InitializeComponent() {
    ::exosnap::startup_log::Write(L"compat: RecordPageT::InitializeComponent entry");
    try {
        if (_contentLoaded) {
            ::exosnap::startup_log::Write(L"compat: RecordPageT already loaded");
            return;
        }

        _contentLoaded = true;
        ::exosnap::startup_log::Write(L"compat: RecordPageT before LoadComponent(RecordPage.xaml)");
        LoadComponent(*this, L"ms-appx:///pages/RecordPage.xaml");
        ::exosnap::startup_log::Write(L"compat: RecordPageT after LoadComponent(RecordPage.xaml)");

        auto root = static_cast<winrt::Microsoft::UI::Xaml::FrameworkElement>(*this);

        ::exosnap::startup_log::Write(L"compat: RecordPageT before FindName batch");
        CapabilityBanner(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::Border>(root, L"CapabilityBanner"));
        CapabilityText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"CapabilityText"));
        TargetCombo(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::ComboBox>(root, L"TargetCombo"));
        OutputPathText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"OutputPathText"));
        StartButton(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::Button>(root, L"StartButton"));
        StopButton(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::Button>(root, L"StopButton"));
        StateText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"StateText"));
        StatsPanel(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::Border>(root, L"StatsPanel"));
        ElapsedText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ElapsedText"));
        FramesCapturedText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"FramesCapturedText"));
        VideoPacketsText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"VideoPacketsText"));
        AudioPacketsText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"AudioPacketsText"));
        DroppedFramesText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"DroppedFramesText"));
        OutputSizeText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"OutputSizeText"));
        ResultPanel(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::Border>(root, L"ResultPanel"));
        ResultStatusText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ResultStatusText"));
        ResultOutputPathText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ResultOutputPathText"));
        ResultErrorPhaseText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ResultErrorPhaseText"));
        ResultHResultText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ResultHResultText"));
        ResultErrorDetailText(FindNameFromElement<winrt::Microsoft::UI::Xaml::Controls::TextBlock>(root, L"ResultErrorDetailText"));
        ::exosnap::startup_log::Write(L"compat: RecordPageT after FindName batch");

        if (auto start = StartButton()) {
            ::exosnap::startup_log::Write(L"compat: RecordPageT wiring StartButton.Click");
            start.Click({static_cast<RecordPage*>(this), &RecordPage::StartButton_Click});
        }
        if (auto stop = StopButton()) {
            ::exosnap::startup_log::Write(L"compat: RecordPageT wiring StopButton.Click");
            stop.Click({static_cast<RecordPage*>(this), &RecordPage::StopButton_Click});
        }
    } catch (winrt::hresult_error const& ex) {
        ::exosnap::startup_log::WriteHResult("compat: RecordPageT::InitializeComponent caught hresult_error", ex);
        throw;
    } catch (...) {
        ::exosnap::startup_log::Write(L"compat: RecordPageT::InitializeComponent caught unknown exception");
        throw;
    }
}

EXO_DEFINE_XAML_COMPAT_STUBS(RecordPageT, RecordPage)

#undef EXO_DEFINE_SIMPLE_XAML_COMPAT
#undef EXO_DEFINE_XAML_COMPAT_STUBS

} // namespace winrt::exosnap::implementation

