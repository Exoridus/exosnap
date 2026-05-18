#include "App.xaml.h"
#include "MainWindow.xaml.h"
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

namespace winrt::exosnap::implementation {

namespace {

void ThrowNoUnloadableObjects() {
    throw winrt::hresult_invalid_argument{L"No unloadable objects."};
}

void ThrowNoUnloadableObjectsToDisconnect() {
    throw winrt::hresult_invalid_argument{L"No unloadable objects to disconnect."};
}

template <typename TObject>
void LoadComponent(TObject const& object, wchar_t const* uri) {
    winrt::Microsoft::UI::Xaml::Application::LoadComponent(
        object,
        winrt::Windows::Foundation::Uri{uri});
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
        int32_t,                                                                                                  \
        winrt::Windows::Foundation::IInspectable const&) {                                                        \
        _contentLoaded = true;                                                                                    \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    winrt::Microsoft::UI::Xaml::Markup::IComponentConnector TEMPLATE_NAME<TYPE_NAME>::GetBindingConnector(       \
        int32_t,                                                                                                  \
        winrt::Windows::Foundation::IInspectable const&) {                                                        \
        return nullptr;                                                                                           \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::UnloadObject(                                                                  \
        winrt::Microsoft::UI::Xaml::DependencyObject const&) {                                                    \
        ThrowNoUnloadableObjects();                                                                               \
    }                                                                                                             \
                                                                                                                  \
    template <>                                                                                                   \
    void TEMPLATE_NAME<TYPE_NAME>::DisconnectUnloadedObject(int32_t) {                                           \
        ThrowNoUnloadableObjectsToDisconnect();                                                                   \
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
    if (!_contentLoaded) {
        _contentLoaded = true;
        winrt::Microsoft::UI::Xaml::Application::LoadComponent(
            *this,
            winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"},
            winrt::Microsoft::UI::Xaml::Controls::Primitives::ComponentResourceLocation::Application);
    }
}

EXO_DEFINE_XAML_COMPAT_STUBS(AppT, App)

template <>
void MainWindowT<MainWindow>::InitializeComponent() {
    if (_contentLoaded) {
        return;
    }

    _contentLoaded = true;
    LoadComponent(*this, L"ms-appx:///MainWindow.xaml");

    NavView(FindNameFromWindow<winrt::Microsoft::UI::Xaml::Controls::NavigationView>(*this, L"NavView"));
    ContentFrame(FindNameFromWindow<winrt::Microsoft::UI::Xaml::Controls::Frame>(*this, L"ContentFrame"));

    if (auto nav = NavView()) {
        nav.SelectionChanged({static_cast<MainWindow*>(this), &MainWindow::NavView_SelectionChanged});
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
    if (_contentLoaded) {
        return;
    }

    _contentLoaded = true;
    LoadComponent(*this, L"ms-appx:///pages/RecordPage.xaml");

    auto root = static_cast<winrt::Microsoft::UI::Xaml::FrameworkElement>(*this);

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

    if (auto start = StartButton()) {
        start.Click({static_cast<RecordPage*>(this), &RecordPage::StartButton_Click});
    }
    if (auto stop = StopButton()) {
        stop.Click({static_cast<RecordPage*>(this), &RecordPage::StopButton_Click});
    }
}

EXO_DEFINE_XAML_COMPAT_STUBS(RecordPageT, RecordPage)

#undef EXO_DEFINE_SIMPLE_XAML_COMPAT
#undef EXO_DEFINE_XAML_COMPAT_STUBS

} // namespace winrt::exosnap::implementation
