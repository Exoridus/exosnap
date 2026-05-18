#include "OutputPage.h"
#include "../startup_log.h"
#if __has_include("pages/OutputPage.xaml.g.hpp")
#include "pages/OutputPage.xaml.g.hpp"
#endif

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::exosnap::implementation {
OutputPage::OutputPage() {
    InitializeComponent();

    if (auto root = this->try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
        container_combo_ = root.FindName(L"ContainerCombo").try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
        audio_codec_combo_ = root.FindName(L"AudioCodecCombo").try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
        mp4_info_text_ = root.FindName(L"Mp4InfoText").try_as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
    }

    if (!container_combo_ || !audio_codec_combo_ || !mp4_info_text_) {
        ::exosnap::startup_log::Write(L"output: failed to resolve named controls");
        return;
    }

    container_combo_.SelectionChanged({this, &OutputPage::ContainerCombo_SelectionChanged});
    container_combo_.SelectedIndex(0);
    ApplyContainerCompatibility();
}

winrt::hstring OutputPage::SelectedItemText(
    winrt::Microsoft::UI::Xaml::Controls::ComboBox const& combo) const {
    if (!combo) {
        return L"";
    }

    if (auto item = combo.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>()) {
        return winrt::unbox_value_or<winrt::hstring>(item.Content(), L"");
    }

    return L"";
}

void OutputPage::SetAudioCodecChoices(bool mp4_mode) {
    if (!audio_codec_combo_) {
        return;
    }

    auto items = audio_codec_combo_.Items();
    items.Clear();

    if (mp4_mode) {
        auto aac = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem{};
        aac.Content(winrt::box_value(winrt::hstring{L"AAC"}));
        items.Append(aac);
        audio_codec_combo_.SelectedIndex(0);
        return;
    }

    auto opus = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem{};
    opus.Content(winrt::box_value(winrt::hstring{L"Opus"}));
    items.Append(opus);

    auto aac = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem{};
    aac.Content(winrt::box_value(winrt::hstring{L"AAC"}));
    items.Append(aac);

    auto target_codec = last_mkv_codec_;
    if (target_codec != L"Opus" && target_codec != L"AAC") {
        target_codec = L"Opus";
    }

    audio_codec_combo_.SelectedIndex(target_codec == L"AAC" ? 1 : 0);
}

void OutputPage::ApplyContainerCompatibility() {
    if (!container_combo_ || !audio_codec_combo_ || !mp4_info_text_) {
        return;
    }

    auto container = SelectedItemText(container_combo_);
    if (container.empty()) {
        container = L"MKV";
    }

    auto selected_codec_before = SelectedItemText(audio_codec_combo_);
    if (container != L"MP4" && (selected_codec_before == L"Opus" || selected_codec_before == L"AAC")) {
        last_mkv_codec_ = selected_codec_before;
    }

    auto mp4_mode = container == L"MP4";
    SetAudioCodecChoices(mp4_mode);
    mp4_info_text_.Visibility(mp4_mode ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                       : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
}

void OutputPage::ContainerCombo_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    ApplyContainerCompatibility();
}
} // namespace winrt::exosnap::implementation

